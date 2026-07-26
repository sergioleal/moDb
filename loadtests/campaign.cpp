#include "campaign.hpp"

#include "budget.hpp"
#include "environments.hpp"
#include "profiles.hpp"
#include "workloads/create_delete_forward.hpp"
#include "workloads/create_delete_interleaved.hpp"
#include "workloads/create_delete_reverse.hpp"
#include "workloads/blob_lifecycle.hpp"
#include "workloads/cascade_delete.hpp"
#include "workloads/create_only.hpp"
#include "workloads/oversubscribed_churn.hpp"
#include "workloads/restart_recovery.hpp"
#include "workloads/crud_full.hpp"
#include "workloads/mixed_oltp.hpp"
#include "workloads/read_hotspot.hpp"
#include "workloads/range_scan_sweep.hpp"
#include "workloads/snapshot_hold.hpp"

#include "json_value.hpp"
#include "runner/environment.hpp"
#include "runner/json_util.hpp"
#include "runner/jsonl_writer.hpp"
#include "runner/sha256.hpp"

#include "modb/net/protocol.hpp"
#include "modb/storage/page_file.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace modb::loadtest {

bool is_unimplemented_error_text(const std::string& error) {
    return error.find("ainda não implementado") != std::string::npos ||
           error.find("ainda não tem dispatch implementado") != std::string::npos ||
           error.find("ainda não tem target implementado") != std::string::npos;
}

namespace {

using modb::bench::collect_environment;
using modb::bench::json_bool;
using modb::bench::json_int;
using modb::bench::json_string;
using modb::bench::json_uint;
using modb::bench::JsonlWriter;
using modb::bench::make_run_id;
using modb::bench::utc_timestamp_millis;

// `filesystem_name` precisa de um caminho absoluto (no Windows, com letra de
// unidade); `work_dir` normalmente chega relativo ("load-results").
std::string effective_filesystem_name(const std::filesystem::path& work_dir) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(work_dir, ec);
    if (ec) {
        return {};
    }
    return modb::bench::filesystem_name(absolute.string());
}

std::string case_params_json(const Case& c) {
    std::ostringstream oss;
    oss << "\"workload\":" << json_string(c.workload) << ",\"target\":" << json_string(c.target)
        << ",\"scale\":" << json_string(c.scale) << ",\"objects\":" << json_uint(c.objects)
        << ",\"environment\":" << json_string(c.environment)
        << ",\"payload\":" << json_string(c.payload) << ",\"batch\":" << json_uint(c.batch)
        << ",\"concurrency\":" << json_uint(c.concurrency) << ",\"readers\":" << json_uint(c.readers)
        << ",\"durability\":" << json_string(c.durability) << ",\"cache\":" << json_string(c.cache)
        << ",\"primary_storage\":" << json_string(c.primary_storage)
        << ",\"repeat_index\":" << json_uint(c.repeat_index)
        << ",\"explicit_variant\":" << json_string(c.explicit_variant);
    return oss.str();
}

std::string phase_json(const PhaseMetrics& p) {
    std::ostringstream oss;
    oss << "\"phase\":" << json_string(p.phase) << ",\"operations\":" << json_uint(p.operations)
        << ",\"duration_ns\":" << json_uint(p.duration_ns)
        << ",\"ops_per_second\":" << p.ops_per_second
        << ",\"bytes_per_object\":" << json_uint(p.bytes_per_object)
        << ",\"errors\":" << json_uint(p.errors) << ",\"latency_ns\":{\"p50\":" << p.latency_ns.p50
        << ",\"p95\":" << p.latency_ns.p95 << ",\"p99\":" << p.latency_ns.p99
        << ",\"p999\":" << p.latency_ns.p999 << "}"
        << ",\"peak_rss_bytes\":" << json_uint(p.peak_rss_bytes)
        << ",\"db_bytes\":" << json_uint(p.db_bytes) << ",\"wal_bytes\":" << json_uint(p.wal_bytes)
        << ",\"pages_read\":" << json_uint(p.pages_read)
        << ",\"pages_written_estimated\":" << json_uint(p.pages_written_estimated)
        << ",\"cache_hit_rate\":" << p.cache_hit_rate
        << ",\"retained_versions\":" << json_uint(p.retained_versions);
    return oss.str();
}

struct CaseTally {
    std::uint64_t completed{};
    std::uint64_t failed{};
    std::uint64_t unimplemented{};
    std::uint64_t skipped_budget{};
};

// Devolve o motivo pelo qual `estimate` excede `limits`, ou "" se cabe (ou se
// a estimativa é desconhecida -- nesse caso não há o que comparar, o gate de
// orçamento desconhecido em run_campaign já cobriu esse caso). Só compara os
// limites que o operador de fato informou (§10: os 3 `--max-*` são opcionais).
std::string exceeds_budget_reason(const BudgetEstimate& estimate, const BudgetLimits& limits) {
    if (!estimate.known) {
        return "";
    }
    if (limits.max_duration_seconds &&
        estimate.duration_ns > *limits.max_duration_seconds * 1'000'000'000ULL) {
        return "duração estimada " + std::to_string(estimate.duration_ns / 1'000'000'000ULL) +
               "s excede --max-duration " + std::to_string(*limits.max_duration_seconds) + "s";
    }
    if (limits.max_disk_gb && estimate.disk_bytes > *limits.max_disk_gb * 1024ULL * 1024 * 1024) {
        return "disco de pico estimado " + std::to_string(estimate.disk_bytes) +
               " bytes excede --max-disk-gb " + std::to_string(*limits.max_disk_gb) + "GB";
    }
    if (limits.max_rss_mb && estimate.peak_rss_bytes > *limits.max_rss_mb * 1024ULL * 1024) {
        return "RSS de pico estimado " + std::to_string(estimate.peak_rss_bytes) +
               " bytes excede --max-rss-mb " + std::to_string(*limits.max_rss_mb) + "MB";
    }
    return "";
}

// Escreve case_start, executa o dispatch (ou grava a recusa quando o
// workload não tem dispatch) e grava progress_window/phase_start/
// phase_summary/case_error|case_summary -- toda a vida de UM caso. Usada por
// `run_campaign` (casos novos) e `resume_campaign` (casos pendentes de um
// `.partial`, Subfase F/§6.4): as duas só diferem em COMO `write_or_fail`
// grava a linha (arquivo novo vs. append num já existente) e em qual
// `sequence` começam a contar -- a lógica de que registros emitir é a mesma.
// Devolve false só quando uma ESCRITA falhou (erro de execução do caso vira
// case_error e não aborta a campanha, como antes desta função existir).
bool run_case_and_record(const Case& c, const std::filesystem::path& work_dir, std::uint64_t seed,
                         const std::string& run_id, std::uint64_t& sequence,
                         const std::function<bool(const std::string&)>& write_or_fail,
                         CaseTally& tally, std::size_t case_index, std::size_t case_count,
                         const CampaignProgressCallback& on_case_progress) {
    const auto case_id = c.case_id();
    const auto case_start_time = std::chrono::steady_clock::now();
    if (on_case_progress) {
        on_case_progress(CampaignProgressEvent{
            .kind = "case_start", .case_index = case_index, .case_count = case_count,
            .case_id = case_id});
    }
    auto fire_case_end = [&](const std::string& status) {
        if (!on_case_progress) {
            return;
        }
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - case_start_time)
                .count());
        on_case_progress(CampaignProgressEvent{
            .kind = "case_end", .case_index = case_index, .case_count = case_count,
            .case_id = case_id, .status = status, .duration_ns = elapsed});
    };
    {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"case_start\","
              "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << ++sequence
            << ",\"case_id\":" << json_string(case_id) << "," << case_params_json(c) << "}";
        if (!write_or_fail(oss.str())) {
            return false;
        }
    }

    if (!is_workload_implemented(c.workload)) {
        ++tally.unimplemented;
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"case_error\","
              "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << ++sequence << ",\"case_id\":"
            << json_string(case_id) << ",\"error\":"
            << json_string("workload '" + c.workload +
                          "' ainda não implementado nesta versão do modb_load (ver "
                          "docs/PLANO_TESTES_DE_CARGA.md §15)")
            << "}";
        fire_case_end("unimplemented");
        return write_or_fail(oss.str());
    }

    // Subfase F (§8 "séries por janela"): cada progress_window emitido pelo
    // target vira uma linha JSONL imediatamente -- não fica acumulado em
    // memória, então uma campanha de 1M não guarda milhares de janelas antes
    // de gravar. Uma falha de escrita aqui marca `progress_write_failed` em
    // vez de abortar o callback (que não pode propagar erro para dentro do
    // target); o caso corrente termina normalmente e o chamador aborta logo
    // depois, como qualquer outra falha de escrita.
    bool progress_write_failed = false;
    ProgressCallback on_progress = [&](const ProgressWindow& w) {
        if (on_case_progress) {
            // Feedback de console, independente do JSONL: uma fase longa
            // (>10s de padrão) fecha janelas periodicamente mesmo quando o
            // caso inteiro leva minutos -- sem isto, o terminal fica mudo
            // do case_start até o case_end de um único caso caro.
            on_case_progress(CampaignProgressEvent{
                .kind = "window", .case_index = case_index, .case_count = case_count,
                .case_id = case_id, .phase = w.phase, .ops_per_second = w.ops_per_second});
        }
        if (progress_write_failed) {
            return;
        }
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":"
              "\"progress_window\",\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << ++sequence
            << ",\"case_id\":" << json_string(case_id) << ",\"phase\":" << json_string(w.phase)
            << ",\"window_index\":" << json_uint(w.window_index)
            << ",\"operations_in_window\":" << json_uint(w.operations_in_window)
            << ",\"elapsed_ns_in_window\":" << json_uint(w.elapsed_ns_in_window)
            << ",\"ops_per_second\":" << w.ops_per_second << ",\"p99_ns\":" << w.p99_ns
            << ",\"peak_rss_bytes\":" << json_uint(w.peak_rss_bytes)
            << ",\"db_bytes\":" << json_uint(w.db_bytes) << "}";
        if (!write_or_fail(oss.str())) {
            progress_write_failed = true;
        }
    };

    std::filesystem::path db_path;
    CaseRunResult run_result;
    if (c.workload == "create_only") {
        run_result = run_create_only(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "create_delete_forward") {
        run_result = run_create_delete_forward(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "create_delete_reverse") {
        run_result = run_create_delete_reverse(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "create_delete_interleaved") {
        run_result = run_create_delete_interleaved(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "crud_full") {
        run_result = run_crud_full(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "read_hotspot") {
        run_result = run_read_hotspot(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "range_scan_sweep") {
        run_result = run_range_scan_sweep(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "mixed_oltp") {
        run_result = run_mixed_oltp(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "snapshot_hold") {
        run_result = run_snapshot_hold(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "blob_lifecycle") {
        run_result = run_blob_lifecycle(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "cascade_delete") {
        run_result = run_cascade_delete(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "oversubscribed_churn") {
        run_result = run_oversubscribed_churn(c, work_dir, seed, on_progress, db_path);
    } else if (c.workload == "restart_recovery") {
        run_result = run_restart_recovery(c, work_dir, seed, on_progress, db_path);
    } else {
        run_result.status = "unimplemented";
        run_result.error = "dispatch ausente para workload '" + c.workload + "'";
    }

    if (progress_write_failed) {
        return false;
    }

    if (run_result.status == "unimplemented") {
        ++tally.unimplemented;
    } else if (!run_result.ok) {
        ++tally.failed;
    } else {
        ++tally.completed;
    }

    for (const auto& phase : run_result.phases) {
        std::ostringstream start_oss;
        start_oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"phase_start\","
                     "\"run_id\":"
                  << json_string(run_id) << ",\"sequence\":" << ++sequence
                  << ",\"case_id\":" << json_string(case_id)
                  << ",\"phase\":" << json_string(phase.phase) << "}";
        if (!write_or_fail(start_oss.str())) {
            return false;
        }
        std::ostringstream summary;
        summary << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":"
                  "\"phase_summary\",\"run_id\":"
                << json_string(run_id) << ",\"sequence\":" << ++sequence
                << ",\"case_id\":" << json_string(case_id) << "," << phase_json(phase) << "}";
        if (!write_or_fail(summary.str())) {
            return false;
        }
    }

    if (!run_result.ok) {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"case_error\","
              "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << ++sequence
            << ",\"case_id\":" << json_string(case_id)
            << ",\"error\":" << json_string(run_result.error) << "}";
        fire_case_end(run_result.status);
        return write_or_fail(oss.str());
    }

    std::ostringstream oss;
    oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"case_summary\","
          "\"run_id\":"
        << json_string(run_id) << ",\"sequence\":" << ++sequence
        << ",\"case_id\":" << json_string(case_id)
        << ",\"status\":" << json_string(run_result.status)
        << ",\"total_duration_ns\":" << json_uint(run_result.total_duration_ns)
        << ",\"peak_disk_bytes\":" << json_uint(run_result.peak_disk_bytes)
        << ",\"expected_hash\":" << json_string(run_result.expected_hash)
        << ",\"actual_hash\":" << json_string(run_result.actual_hash)
        << ",\"hash_match\":" << json_bool(run_result.hash_match)
        << ",\"write_amplification\":" << run_result.write_amplification
        << ",\"space_amplification\":" << run_result.space_amplification
        << ",\"all_deleted\":" << json_bool(run_result.all_deleted)
        << ",\"still_resolving\":" << json_uint(run_result.still_resolving)
        << ",\"reclaimed_bytes\":" << json_uint(run_result.reclaimed_bytes)
        << ",\"db_path\":" << json_string(db_path.string()) << ",\"windows\":";
    if (run_result.windows.has_windows) {
        oss << "{\"first_ops_per_second\":" << run_result.windows.first_ops_per_second
            << ",\"last_ops_per_second\":" << run_result.windows.last_ops_per_second
            << ",\"slope_ops_per_second_per_min\":" << run_result.windows.slope_ops_per_second_per_min
            << ",\"first_p99_ns\":" << run_result.windows.first_p99_ns
            << ",\"last_p99_ns\":" << run_result.windows.last_p99_ns << "}";
    } else {
        oss << "null";
    }
    oss << "}";
    fire_case_end(run_result.status);
    return write_or_fail(oss.str());
}

// Reconstrói um `Case` a partir do record `case_start` já gravado no
// `.partial` (mesmos campos de `case_params_json`, na direção inversa) --
// usado só por `resume_campaign`. Nunca via `parse_case_id`: o id só
// codifica as dimensões secundárias que saem do padrão como um sufixo opaco
// (payload_fat, batch500, ...), então reconstruir a partir do texto do id
// perderia informação que `case_start` já carrega com nome de campo.
Case case_from_case_start(const JsonValue& v) {
    Case c;
    c.workload = v.get_string("workload");
    c.target = v.get_string("target");
    c.scale = v.get_string("scale");
    c.objects = static_cast<std::uint64_t>(v.get_number("objects"));
    c.environment = v.get_string("environment");
    c.payload = v.get_string("payload");
    c.batch = static_cast<std::uint64_t>(v.get_number("batch"));
    c.concurrency = static_cast<std::uint64_t>(v.get_number("concurrency"));
    c.readers = static_cast<std::uint64_t>(v.get_number("readers"));
    c.durability = v.get_string("durability");
    c.cache = v.get_string("cache");
    c.primary_storage = v.get_string("primary_storage");
    c.repeat_index = static_cast<std::uint64_t>(v.get_number("repeat_index"));
    c.explicit_variant = v.get_string("explicit_variant");
    return c;
}

// Nome canônico do arquivo de campanha (§13.3): schema próprio "modb-load-",
// não o `make_result_filename` de modb_bench (que é "modb-benchmark-").
// Sufixo monotônico -01/-02 se o nome já existir (§4.1 do plano de
// benchmarks, reaproveitado sem divergência).
std::filesystem::path make_load_result_filename(const std::filesystem::path& dir,
                                                const std::string& utc_stamp,
                                                const std::string& commit_short,
                                                const std::string& host_token) {
    const std::string base = "modb-load-" + utc_stamp + "-" + commit_short + "-" + host_token;
    std::filesystem::path candidate = dir / (base + ".jsonl");
    if (!std::filesystem::exists(candidate)) {
        return candidate;
    }
    for (int suffix = 1; suffix < 100; ++suffix) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02d", suffix);
        candidate = dir / (base + "-" + buf + ".jsonl");
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return candidate; // extremamente improvável; deixa o open() final falhar com clareza
}

} // namespace

ResolveResult resolve_cases(const CampaignOptions& options) {
    ResolveResult result;

    std::vector<Case> profile_cases;
    if (!options.selectors.case_ids.empty()) {
        // `--case` substitui o perfil por completo (§6.1); expand_matrix já
        // trata isso quando profile_cases fica vazio e case_ids não está.
    } else if (options.profile) {
        auto profile = find_profile(*options.profile);
        if (!profile) {
            result.error = "perfil desconhecido: " + *options.profile;
            return result;
        }
        profile_cases = profile->cases;
    } else {
        result.error = "nenhum --profile nem --case informado";
        return result;
    }

    auto expanded = expand_matrix(profile_cases, options.selectors);
    if (!expanded.error.empty()) {
        result.error = expanded.error;
        return result;
    }

    // Valida ambientes citados contra o catálogo, se ele existir. Ausência do
    // catálogo não é erro aqui -- environments.json é opcional para quem só
    // quer rodar embedded sem cadastrar nada.
    if (std::filesystem::exists(options.environments_file)) {
        auto loaded = load_environment_catalog(options.environments_file);
        if (!loaded.ok) {
            result.error = loaded.error;
            return result;
        }
        for (const auto& c : expanded.cases) {
            if (c.environment.empty()) {
                continue;
            }
            auto validated = validate_environment(loaded.catalog, c.environment, /*local_only=*/true);
            if (!validated.ok) {
                result.error = validated.error;
                return result;
            }
        }
    }

    result.ok = true;
    result.cases = std::move(expanded.cases);
    return result;
}

std::string render_case_plan(const std::vector<Case>& cases, const CalibrationTable& table) {
    std::ostringstream oss;
    std::uint64_t total_objects = 0;
    std::uint64_t total_disk_known = 0;
    std::uint64_t total_duration_known = 0;
    bool any_unknown = false;
    for (const auto& c : cases) {
        auto estimate = estimate_case(c, table);
        oss << c.case_id() << "  objetos=" << c.objects << "  disco~="
            << (estimate.known ? std::to_string(estimate.disk_bytes) : "?") << "  tempo~="
            << (estimate.known ? std::to_string(estimate.duration_ns) : "?") << '\n';
        total_objects += c.objects;
        if (estimate.known) {
            // Casos não limpam seus próprios arquivos de banco entre si
            // (cada um gera um nome único em work_dir) -- o disco realmente
            // consumido por uma campanha é a SOMA dos picos, não o maior
            // caso isolado.
            total_disk_known += estimate.disk_bytes;
            total_duration_known += estimate.duration_ns;
        } else {
            any_unknown = true;
        }
    }
    const std::string prefix = any_unknown ? ">=" : "";
    const std::string suffix = any_unknown ? " (parcial -- ao menos 1 caso sem calibração)" : "";
    oss << "--\n"
        << cases.size() << " caso(s)  objetos totais=" << total_objects
        << "  disco de pico estimado ~= " << prefix << total_disk_known << suffix
        << "  tempo total estimado ~= " << prefix << total_duration_known << suffix << '\n';
    return oss.str();
}

CampaignResult run_campaign(const CampaignOptions& options) {
    CampaignResult result;

    auto resolved = resolve_cases(options);
    if (!resolved.ok) {
        result.error = resolved.error;
        return result;
    }

    const auto calibration_path =
        options.calibration_file.empty() ? default_calibration_path() : options.calibration_file;
    auto calibration_loaded = load_calibration(calibration_path);
    if (!calibration_loaded.ok) {
        result.error = calibration_loaded.error;
        return result;
    }
    const auto& calibration = calibration_loaded.table;

    if (options.dry_run) {
        result.ok = true;
        result.status = "dry_run";
        result.rendered_plan = render_case_plan(resolved.cases, calibration);
        return result;
    }

    // Gate original (§6.3): sem NENHUMA estimativa para um caso, `run` exige
    // aceitação explícita do operador -- a tabela de calibração (Subfase H)
    // não cobre toda combinação (workload,payload,scale), então esse gate
    // continua vivo mesmo com a tabela preenchida.
    bool any_unknown_estimate = false;
    std::uint64_t total_known_disk_bytes = 0;
    for (const auto& c : resolved.cases) {
        const auto estimate = estimate_case(c, calibration);
        if (estimate.known) {
            total_known_disk_bytes += estimate.disk_bytes;
        } else {
            any_unknown_estimate = true;
        }
    }
    auto budget_check = check_campaign_budget(options.budget, any_unknown_estimate);
    if (!budget_check.ok) {
        result.error = budget_check.reason;
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(options.output_dir, ec);
    const std::filesystem::path work_dir =
        options.work_dir.empty() ? options.output_dir : options.work_dir;
    std::filesystem::create_directories(work_dir, ec);

    // §10: "run verifica espaço livre e aborta com mensagem clara antes de
    // começar, em vez de encher o disco no meio de 1M". Casos não limpam seu
    // próprio arquivo entre si (nomes únicos por caso em work_dir), então o
    // espaço necessário é a SOMA dos picos conhecidos -- casos sem
    // calibração não entram na soma (não há como), o gate acima já cobre
    // esse caso via --accept-unknown-budget.
    if (total_known_disk_bytes > 0) {
        std::error_code space_ec;
        const auto space_info = std::filesystem::space(work_dir, space_ec);
        if (!space_ec && space_info.available < total_known_disk_bytes) {
            result.error = "espaço livre insuficiente em " + work_dir.string() + ": disponível=" +
                           std::to_string(space_info.available) + " bytes, estimado=" +
                           std::to_string(total_known_disk_bytes) +
                           " bytes (soma dos picos calibrados dos casos do plano)";
            return result;
        }
    }

    const auto env_info = collect_environment(options.argv_joined);
    const auto stamp = utc_timestamp_millis();
    const auto run_id = make_run_id(stamp);
    result.run_id = run_id;

    const auto result_path =
        make_load_result_filename(options.output_dir, stamp, env_info.git_commit_short,
                                  env_info.hostname_token);
    result.result_path = result_path;

    JsonlWriter writer;
    if (!writer.open(result_path)) {
        result.error = "não foi possível abrir " + result_path.string() + ".partial para escrita";
        return result;
    }

    auto write_or_fail = [&](const std::string& line) -> bool {
        if (!writer.write_line(line)) {
            result.error = "falha ao escrever em " + writer.partial_path().string();
            writer.abandon();
            return false;
        }
        return true;
    };

    std::uint64_t sequence = 0;

    {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"run_start\","
              "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << ++sequence
            << ",\"started_at\":" << json_string(stamp) << ",\"profile\":"
            << json_string(options.profile.value_or("")) << ",\"seed\":"
            << json_string(std::to_string(options.seed))
            << ",\"command\":" << json_string(options.argv_joined) << "}";
        if (!write_or_fail(oss.str())) {
            return result;
        }
    }
    {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"environment\","
              "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << ++sequence
            << ",\"git_commit\":" << json_string(env_info.git_commit)
            << ",\"git_commit_short\":" << json_string(env_info.git_commit_short)
            << ",\"git_branch\":" << json_string(env_info.git_branch)
            << ",\"git_dirty\":" << json_bool(env_info.git_dirty)
            << ",\"compiler_id\":" << json_string(env_info.compiler_id)
            << ",\"compiler_version\":" << json_string(env_info.compiler_version)
            << ",\"cxx_standard\":" << json_string(env_info.cxx_standard)
            << ",\"build_type\":" << json_string(env_info.build_type)
            << ",\"instrumentation\":" << json_string(env_info.instrumentation)
            << ",\"os_name\":" << json_string(env_info.os_name)
            << ",\"os_version\":" << json_string(env_info.os_version)
            << ",\"arch\":" << json_string(env_info.arch)
            << ",\"cpu_model\":" << json_string(env_info.cpu_model)
            << ",\"cores_physical\":" << json_uint(env_info.cores_physical)
            << ",\"cores_logical\":" << json_uint(env_info.cores_logical)
            << ",\"ram_bytes\":" << json_uint(env_info.ram_bytes)
            << ",\"fs\":" << json_string(effective_filesystem_name(work_dir))
            << ",\"hostname_token\":" << json_string(env_info.hostname_token)
            // Numérico, não string: o rollup lê com get_number e uma string
            // virava page_size=0 -- inclusive dentro da series_key, colidindo
            // corridas de 4k/8k/16k numa série só
            // (docs-process/PLANO_PROFILING.md §3, M3).
            << ",\"page_size\":" << env_info.page_size
            << ",\"project_version\":" << json_string(env_info.project_version)
            << ",\"format_version\":" << json_uint(modb::storage::current_format_version)
            << ",\"protocol_version\":" << json_uint(modb::net::protocol_version) << "}";
        if (!write_or_fail(oss.str())) {
            return result;
        }
    }
    {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"case_plan\","
              "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << ++sequence
            << ",\"case_count\":" << json_uint(resolved.cases.size()) << ",\"cases\":[";
        for (std::size_t i = 0; i < resolved.cases.size(); ++i) {
            if (i > 0) {
                oss << ',';
            }
            oss << json_string(resolved.cases[i].case_id());
        }
        oss << "]}";
        if (!write_or_fail(oss.str())) {
            return result;
        }
    }

    CaseTally tally;
    const auto case_count = resolved.cases.size();
    for (std::size_t case_index = 0; case_index < case_count; ++case_index) {
        const auto& c = resolved.cases[case_index];
        const auto case_id = c.case_id();
        const auto estimate = estimate_case(c, calibration);
        const auto reason = exceeds_budget_reason(estimate, options.budget);
        if (!reason.empty()) {
            ++tally.skipped_budget;
            std::ostringstream oss;
            oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"skipped_budget\","
                  "\"run_id\":"
                << json_string(run_id) << ",\"sequence\":" << ++sequence
                << ",\"case_id\":" << json_string(case_id) << ",\"reason\":" << json_string(reason)
                << ",\"estimated_disk_bytes\":" << json_uint(estimate.disk_bytes)
                << ",\"estimated_duration_ns\":" << json_uint(estimate.duration_ns)
                << ",\"estimated_peak_rss_bytes\":" << json_uint(estimate.peak_rss_bytes) << "}";
            if (options.on_progress) {
                options.on_progress(CampaignProgressEvent{
                    .kind = "skipped", .case_index = case_index, .case_count = case_count,
                    .case_id = case_id, .status = reason});
            }
            if (!write_or_fail(oss.str())) {
                return result;
            }
            continue;
        }
        if (!run_case_and_record(c, work_dir, options.seed, run_id, sequence, write_or_fail, tally,
                                 case_index, case_count, options.on_progress)) {
            return result;
        }
    }

    const std::string overall_status =
        tally.failed > 0
            ? "failed"
            : (tally.unimplemented > 0 || tally.skipped_budget > 0 ? "partial" : "completed");
    {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"run_end\","
              "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << ++sequence
            << ",\"status\":" << json_string(overall_status)
            << ",\"completed\":" << json_uint(tally.completed)
            << ",\"failed\":" << json_uint(tally.failed)
            << ",\"unimplemented\":" << json_uint(tally.unimplemented)
            << ",\"skipped_budget\":" << json_uint(tally.skipped_budget)
            << ",\"previous_content_sha256\":" << json_string(writer.content_sha256_hex()) << "}";
        if (!write_or_fail(oss.str())) {
            return result;
        }
    }

    if (!writer.finish()) {
        result.error = "não foi possível promover " + writer.partial_path().string() + " para " +
                       writer.final_path().string();
        return result;
    }

    result.ok = overall_status != "failed";
    result.status = overall_status;
    return result;
}

CampaignResult resume_campaign(const ResumeOptions& options) {
    CampaignResult result;

    if (!std::filesystem::exists(options.partial_path)) {
        result.error = "arquivo não encontrado: " + options.partial_path.string();
        return result;
    }
    const std::string partial_str = options.partial_path.string();
    const std::string kSuffix = ".partial";
    if (partial_str.size() <= kSuffix.size() ||
        partial_str.compare(partial_str.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        result.error = "resume espera um caminho terminado em .partial, recebeu: " + partial_str;
        return result;
    }
    const std::filesystem::path final_path = partial_str.substr(0, partial_str.size() - kSuffix.size());
    if (std::filesystem::exists(final_path)) {
        result.error = "arquivo final já existe, não é seguro sobrescrever: " + final_path.string();
        return result;
    }

    std::ifstream in(options.partial_path, std::ios::binary);
    if (!in) {
        result.error = "não foi possível abrir " + partial_str + " para leitura";
        return result;
    }
    std::ostringstream in_buffer;
    in_buffer << in.rdbuf();
    std::string content = in_buffer.str();
    in.close();

    // Varre o arquivo uma vez: run_id/seed (run_start), a ordem oficial dos
    // casos (case_plan) e o veredito de cada OCORRÊNCIA já resolvida
    // (case_summary/case_error/skipped_budget). `sequence` continua de onde o
    // arquivo parou, nunca reinicia em 0 -- senão duas linhas do mesmo
    // arquivo colidiriam de sequência.
    //
    // Identidade por posição, não por case_id: `--repeat N` produz N
    // registros com o MESMO case_id (repeat_index não participa de
    // Case::case_id(), §5), então um `std::set<std::string>`/`std::map<
    // std::string, ...>` chaveado por case_id trataria a repetição 2 como
    // "já resolvida" assim que a repetição 1 terminasse. `run_case_and_record`
    // processa um caso por vez (case_start...veredito) antes do próximo, e
    // `skipped_budget` ocupa uma posição do plano sem emitir case_start --
    // então um contador sequencial de "quantas ocorrências já vimos" (uma por
    // case_start OU skipped_budget) identifica cada ocorrência sem ambiguidade
    // e sem depender do texto do case_id.
    std::string run_id, seed_str;
    std::vector<std::string> case_plan_ids;
    std::map<std::size_t, JsonValue> case_starts_by_position;
    std::set<std::size_t> resolved_positions;
    std::size_t next_position = 0;
    std::size_t current_position = 0;
    bool have_current_position = false;
    std::uint64_t sequence = 0;
    CaseTally tally;

    // Pré-divide em linhas não vazias (em vez de um `while (getline)` direto)
    // para poder distinguir "a ÚLTIMA linha está torta" de "uma linha no
    // meio está corrompida": só a última pode ser uma escrita truncada por
    // uma queda de processo no meio de um fwrite -- exatamente o cenário
    // que `resume` existe para cobrir -- então é tratada como fim de
    // arquivo, não como corrupção real. Uma linha malformada no MEIO
    // continua um erro rígido (algo genuinamente errado, não uma
    // interrupção limpa).
    std::vector<std::string> raw_lines;
    {
        std::istringstream lines(content);
        std::string raw_line;
        while (std::getline(lines, raw_line)) {
            if (!raw_line.empty()) {
                raw_lines.push_back(raw_line);
            }
        }
    }

    for (std::size_t i = 0; i < raw_lines.size(); ++i) {
        auto parsed = parse_json(raw_lines[i]);
        if (!parsed.ok || !parsed.value.is_object()) {
            if (i + 1 == raw_lines.size()) {
                break;
            }
            result.error = partial_str + ":" + std::to_string(i + 1) +
                           ": linha não é um objeto JSON válido (" + parsed.error + ")";
            return result;
        }
        const auto& v = parsed.value;
        sequence = std::max(sequence, static_cast<std::uint64_t>(v.get_number("sequence")));
        const auto record = v.get_string("record");
        if (record == "run_start") {
            run_id = v.get_string("run_id");
            seed_str = v.get_string("seed");
        } else if (record == "case_plan") {
            const auto* cases = v.find("cases");
            if (cases && cases->is_array()) {
                for (const auto& item : cases->as_array()) {
                    if (item.is_string()) {
                        case_plan_ids.push_back(item.as_string());
                    }
                }
            }
        } else if (record == "case_start") {
            current_position = next_position++;
            have_current_position = true;
            case_starts_by_position[current_position] = v;
        } else if (record == "case_summary") {
            if (!have_current_position) {
                continue;   // arquivo corrompido/fora de ordem -- defensivo
            }
            resolved_positions.insert(current_position);
            if (v.get_string("status") == "completed") {
                ++tally.completed;
            } else {
                ++tally.failed;
            }
        } else if (record == "case_error") {
            if (!have_current_position) {
                continue;
            }
            if (resolved_positions.insert(current_position).second) {
                const auto error_text = v.get_string("error");
                if (is_unimplemented_error_text(error_text)) {
                    ++tally.unimplemented;
                } else {
                    ++tally.failed;
                }
            }
        } else if (record == "skipped_budget") {
            // Terceiro estado terminal de uma ocorrência (além de
            // case_summary/case_error): a decisão de pular já foi tomada e
            // registrada, e um caso pulado nunca tem case_start (o skip
            // acontece antes) -- por isso ocupa sua própria posição no plano
            // em vez de reusar `current_position` de uma ocorrência anterior.
            current_position = next_position++;
            have_current_position = true;
            if (resolved_positions.insert(current_position).second) {
                ++tally.skipped_budget;
            }
        }
    }

    if (run_id.empty() || case_plan_ids.empty()) {
        result.error = partial_str + " não contém run_start/case_plan válidos -- não é um "
                       ".partial de modb_load reconhecível";
        return result;
    }

    std::vector<Case> pending_cases;
    for (std::size_t position = 0; position < case_plan_ids.size(); ++position) {
        if (resolved_positions.contains(position)) {
            continue;
        }
        auto it = case_starts_by_position.find(position);
        if (it == case_starts_by_position.end()) {
            result.error = "caso '" + case_plan_ids[position] + "' (posição " +
                           std::to_string(position) +
                           " do plano) foi interrompido antes de emitir case_start -- não há "
                           "parâmetros efetivos gravados para retomar com segurança; rode-o "
                           "isolado via --case";
            return result;
        }
        pending_cases.push_back(case_from_case_start(it->second));
    }

    if (pending_cases.empty()) {
        result.error = "todos os casos do plano já têm veredito (case_summary/case_error) -- nada "
                       "para retomar; se a escrita de run_end falhou, remova o sufixo .partial "
                       "manualmente após conferir o conteúdo";
        return result;
    }

    const std::uint64_t seed =
        options.seed_override != 0 ? options.seed_override : std::strtoull(seed_str.c_str(), nullptr, 10);
    const std::filesystem::path work_dir =
        options.work_dir.empty() ? options.partial_path.parent_path() : options.work_dir;
    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);

    // Reaplica --max-* aos casos pendentes (§10) -- sem isto, um `resume`
    // perderia o orçamento que a campanha original tinha, cada vez que fosse
    // retomada, mesmo que o operador tenha passado os mesmos --max-* de novo.
    const auto calibration_path = options.calibration_file.empty() ? default_calibration_path()
                                                                    : options.calibration_file;
    auto calibration_loaded = load_calibration(calibration_path);
    if (!calibration_loaded.ok) {
        result.error = calibration_loaded.error;
        return result;
    }
    const auto& calibration = calibration_loaded.table;

    std::ofstream out(options.partial_path, std::ios::binary | std::ios::app);
    if (!out) {
        result.error = "não foi possível reabrir " + partial_str + " para acrescentar";
        return result;
    }
    std::string write_error;
    auto write_or_fail = [&](const std::string& line) -> bool {
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        out.put('\n');
        out.flush();
        if (!out) {
            write_error = "falha ao acrescentar em " + partial_str;
            return false;
        }
        content.append(line);
        content.push_back('\n');
        return true;
    };

    const auto pending_count = pending_cases.size();
    for (std::size_t pending_index = 0; pending_index < pending_count; ++pending_index) {
        const auto& c = pending_cases[pending_index];
        const auto case_id = c.case_id();
        const auto estimate = estimate_case(c, calibration);
        const auto reason = exceeds_budget_reason(estimate, options.budget);
        if (!reason.empty()) {
            ++tally.skipped_budget;
            std::ostringstream oss;
            oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"skipped_budget\","
                  "\"run_id\":"
                << json_string(run_id) << ",\"sequence\":" << ++sequence
                << ",\"case_id\":" << json_string(case_id) << ",\"reason\":" << json_string(reason)
                << ",\"estimated_disk_bytes\":" << json_uint(estimate.disk_bytes)
                << ",\"estimated_duration_ns\":" << json_uint(estimate.duration_ns)
                << ",\"estimated_peak_rss_bytes\":" << json_uint(estimate.peak_rss_bytes) << "}";
            if (options.on_progress) {
                options.on_progress(CampaignProgressEvent{
                    .kind = "skipped", .case_index = pending_index, .case_count = pending_count,
                    .case_id = case_id, .status = reason});
            }
            if (!write_or_fail(oss.str())) {
                result.error = write_error;
                return result;
            }
            continue;
        }
        if (!run_case_and_record(c, work_dir, seed, run_id, sequence, write_or_fail, tally,
                                 pending_index, pending_count, options.on_progress)) {
            result.error = write_error;
            return result;
        }
    }

    const std::string overall_status =
        tally.failed > 0
            ? "failed"
            : (tally.unimplemented > 0 || tally.skipped_budget > 0 ? "partial" : "completed");
    {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"run_end\","
              "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << ++sequence
            << ",\"status\":" << json_string(overall_status)
            << ",\"completed\":" << json_uint(tally.completed)
            << ",\"failed\":" << json_uint(tally.failed)
            << ",\"unimplemented\":" << json_uint(tally.unimplemented)
            << ",\"skipped_budget\":" << json_uint(tally.skipped_budget) << ",\"previous_content_sha256\":"
            << json_string(modb::bench::sha256_hex(modb::bench::sha256_text(content))) << "}";
        if (!write_or_fail(oss.str())) {
            result.error = write_error;
            return result;
        }
    }

    out.close();
    std::filesystem::rename(options.partial_path, final_path, ec);
    if (ec) {
        result.error =
            "não foi possível promover " + partial_str + " para " + final_path.string();
        return result;
    }

    result.ok = overall_status != "failed";
    result.status = overall_status;
    result.run_id = run_id;
    result.result_path = final_path;
    return result;
}

} // namespace modb::loadtest
