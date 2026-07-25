#include "campaign.hpp"

#include "budget.hpp"
#include "environments.hpp"
#include "profiles.hpp"
#include "workloads/create_delete_forward.hpp"
#include "workloads/create_delete_interleaved.hpp"
#include "workloads/create_delete_reverse.hpp"
#include "workloads/create_only.hpp"
#include "workloads/crud_full.hpp"

#include "runner/environment.hpp"
#include "runner/json_util.hpp"
#include "runner/jsonl_writer.hpp"

#include "modb/net/protocol.hpp"
#include "modb/storage/page_file.hpp"

#include <sstream>
#include <system_error>

namespace modb::loadtest {
namespace {

using modb::bench::collect_environment;
using modb::bench::json_bool;
using modb::bench::json_int;
using modb::bench::json_string;
using modb::bench::json_uint;
using modb::bench::JsonlWriter;
using modb::bench::make_run_id;
using modb::bench::utc_timestamp_millis;

std::string case_params_json(const Case& c) {
    std::ostringstream oss;
    oss << "\"workload\":" << json_string(c.workload) << ",\"target\":" << json_string(c.target)
        << ",\"scale\":" << json_string(c.scale) << ",\"objects\":" << json_uint(c.objects)
        << ",\"environment\":" << json_string(c.environment)
        << ",\"payload\":" << json_string(c.payload) << ",\"batch\":" << json_uint(c.batch)
        << ",\"concurrency\":" << json_uint(c.concurrency) << ",\"readers\":" << json_uint(c.readers)
        << ",\"durability\":" << json_string(c.durability) << ",\"cache\":" << json_string(c.cache)
        << ",\"primary_storage\":" << json_string(c.primary_storage)
        << ",\"repeat_index\":" << json_uint(c.repeat_index);
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
        << ",\"pages_written_estimated\":" << json_uint(p.pages_written_estimated);
    return oss.str();
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

std::string render_case_plan(const std::vector<Case>& cases) {
    std::ostringstream oss;
    std::uint64_t total_objects = 0;
    for (const auto& c : cases) {
        auto estimate = estimate_case(c);
        oss << c.case_id() << "  objetos=" << c.objects << "  disco~="
            << (estimate.known ? std::to_string(estimate.disk_bytes) : "?") << "  tempo~="
            << (estimate.known ? std::to_string(estimate.duration_ns) : "?") << '\n';
        total_objects += c.objects;
    }
    oss << "--\n"
        << cases.size() << " caso(s)  objetos totais=" << total_objects
        << "  disco de pico estimado ~= ?  tempo total estimado ~= ?\n";
    return oss.str();
}

CampaignResult run_campaign(const CampaignOptions& options) {
    CampaignResult result;

    auto resolved = resolve_cases(options);
    if (!resolved.ok) {
        result.error = resolved.error;
        return result;
    }

    if (options.dry_run) {
        result.ok = true;
        result.status = "dry_run";
        result.rendered_plan = render_case_plan(resolved.cases);
        return result;
    }

    // Nenhuma tabela de calibração ainda (§10): toda estimativa é
    // desconhecida, então este gate dispara sempre que `run` de fato tenta
    // executar, a menos que o operador aceite explicitamente.
    auto budget_check = check_campaign_budget(options.budget, /*any_unknown_estimate=*/true);
    if (!budget_check.ok) {
        result.error = budget_check.reason;
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(options.output_dir, ec);
    const std::filesystem::path work_dir =
        options.work_dir.empty() ? options.output_dir : options.work_dir;
    std::filesystem::create_directories(work_dir, ec);

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

    {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"run_start\","
              "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
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
            << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
            << ",\"git_commit\":" << json_string(env_info.git_commit)
            << ",\"git_commit_short\":" << json_string(env_info.git_commit_short)
            << ",\"git_branch\":" << json_string(env_info.git_branch)
            << ",\"git_dirty\":" << json_bool(env_info.git_dirty)
            << ",\"compiler_id\":" << json_string(env_info.compiler_id)
            << ",\"compiler_version\":" << json_string(env_info.compiler_version)
            << ",\"cxx_standard\":" << json_string(env_info.cxx_standard)
            << ",\"build_type\":" << json_string(env_info.build_type)
            << ",\"os_name\":" << json_string(env_info.os_name)
            << ",\"os_version\":" << json_string(env_info.os_version)
            << ",\"arch\":" << json_string(env_info.arch)
            << ",\"hostname_token\":" << json_string(env_info.hostname_token)
            << ",\"page_size\":" << json_string(env_info.page_size)
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
            << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
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

    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint64_t unimplemented = 0;

    for (const auto& c : resolved.cases) {
        const auto case_id = c.case_id();
        {
            std::ostringstream oss;
            oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"case_start\","
                  "\"run_id\":"
                << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
                << ",\"case_id\":" << json_string(case_id) << "," << case_params_json(c) << "}";
            if (!write_or_fail(oss.str())) {
                return result;
            }
        }

        if (!is_workload_implemented(c.workload)) {
            ++unimplemented;
            std::ostringstream oss;
            oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"case_error\","
                  "\"run_id\":"
                << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
                << ",\"case_id\":" << json_string(case_id) << ",\"error\":"
                << json_string("workload '" + c.workload +
                              "' ainda não implementado nesta versão do modb_load (ver "
                              "docs/PLANO_TESTES_DE_CARGA.md §15)")
                << "}";
            if (!write_or_fail(oss.str())) {
                return result;
            }
            continue;
        }

        std::filesystem::path db_path;
        CaseRunResult run_result;
        if (c.workload == "create_only") {
            run_result = run_create_only(c, work_dir, options.seed, db_path);
        } else if (c.workload == "create_delete_forward") {
            run_result = run_create_delete_forward(c, work_dir, options.seed, db_path);
        } else if (c.workload == "create_delete_reverse") {
            run_result = run_create_delete_reverse(c, work_dir, options.seed, db_path);
        } else if (c.workload == "create_delete_interleaved") {
            run_result = run_create_delete_interleaved(c, work_dir, options.seed, db_path);
        } else if (c.workload == "crud_full") {
            run_result = run_crud_full(c, work_dir, options.seed, db_path);
        } else {
            run_result.status = "unimplemented";
            run_result.error = "dispatch ausente para workload '" + c.workload + "'";
        }

        if (run_result.status == "unimplemented") {
            ++unimplemented;
        } else if (!run_result.ok) {
            ++failed;
        } else {
            ++completed;
        }

        for (const auto& phase : run_result.phases) {
            std::ostringstream oss;
            oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"phase_start\","
                  "\"run_id\":"
                << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
                << ",\"case_id\":" << json_string(case_id)
                << ",\"phase\":" << json_string(phase.phase) << "}";
            if (!write_or_fail(oss.str())) {
                return result;
            }
            std::ostringstream summary;
            summary << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":"
                      "\"phase_summary\",\"run_id\":"
                    << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
                    << ",\"case_id\":" << json_string(case_id) << "," << phase_json(phase) << "}";
            if (!write_or_fail(summary.str())) {
                return result;
            }
        }

        if (!run_result.ok) {
            std::ostringstream oss;
            oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"case_error\","
                  "\"run_id\":"
                << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
                << ",\"case_id\":" << json_string(case_id)
                << ",\"error\":" << json_string(run_result.error) << "}";
            if (!write_or_fail(oss.str())) {
                return result;
            }
            continue;
        }

        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"case_summary\","
              "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
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
            << ",\"db_path\":" << json_string(db_path.string()) << "}";
        if (!write_or_fail(oss.str())) {
            return result;
        }
    }

    const std::string overall_status = failed > 0 ? "failed" : (unimplemented > 0 ? "partial" : "completed");
    {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest\",\"schema_version\":1,\"record\":\"run_end\","
              "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
            << ",\"status\":" << json_string(overall_status)
            << ",\"completed\":" << json_uint(completed) << ",\"failed\":" << json_uint(failed)
            << ",\"unimplemented\":" << json_uint(unimplemented)
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

} // namespace modb::loadtest
