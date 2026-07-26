#include "history/rollup.hpp"

#include "campaign.hpp"
#include "environments.hpp"
#include "history/series_key.hpp"
#include "json_value.hpp"
#include "runner/json_util.hpp"
#include "runner/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace modb::loadtest {
namespace {

using modb::bench::json_bool;
using modb::bench::json_string;
using modb::bench::json_uint;

// Campo ausente vira `null`, nunca "" nem 0 inventado (§13.3): um zero
// indistinguível de "medi e deu zero" envenena a série anos depois.
std::string string_or_null(const std::string& value) {
    return value.empty() ? "null" : json_string(value);
}

std::string uint_or_null(std::uint64_t value) {
    return value == 0 ? "null" : json_uint(value);
}

// RAM em GB inteiros, como o schema documenta (`ram_gb`); arredonda para o mais
// próximo porque o total físico reportado nunca é um múltiplo exato.
std::string ram_gb_or_null(std::uint64_t ram_bytes) {
    if (ram_bytes == 0) {
        return "null";
    }
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    return json_uint((ram_bytes + gib / 2) / gib);
}

// Estado acumulado de um `case_id` enquanto a campanha é varrida linha a
// linha -- os records de um caso não são contíguos no arquivo (case_start,
// phase_start/phase_summary por fase, depois case_summary ou case_error).
struct CaseAccumulator {
    std::string workload, target, scale, environment, payload, durability, cache, primary_storage;
    std::uint64_t objects{}, batch{}, concurrency{}, readers{}, repeat_index{};
    std::vector<std::string> phase_json_objects;   // phase_summary já formatado como objeto JSON
    std::uint64_t peak_rss_bytes_across_phases{};   // máximo entre as fases, para `totals`

    bool has_summary{false};
    bool hash_match{false};
    std::string status, expected_hash, actual_hash, db_path, error_message;
    double total_duration_ns{}, peak_disk_bytes{}, write_amplification{}, space_amplification{};
    double reclaimed_bytes{};

    // Subfase F (§13.3 `windows`): ausente (has_windows=false) quando nenhuma
    // fase do caso durou o bastante para fechar uma janela.
    bool has_windows{false};
    double windows_first_ops_per_second{}, windows_last_ops_per_second{};
    double windows_slope_ops_per_second_per_min{};
    double windows_first_p99_ns{}, windows_last_p99_ns{};

    bool has_error_only{false};   // case_error sem case_summary
};

std::string phase_summary_object(const JsonValue& line) {
    // O record já carrega exatamente os campos do rollup (mesmos nomes,
    // §13.3) -- repassa como está, só troca pages_written_estimated pelo
    // nome canônico pages_written (a estimativa é a única fonte que existe
    // hoje; ver docs-process/PLANO_IMPLEMENTACAO_CARGA.md, Subfase D2).
    std::ostringstream oss;
    oss << "{\"phase\":" << json_string(line.get_string("phase"))
        << ",\"operations\":" << json_uint(static_cast<std::uint64_t>(line.get_number("operations")))
        << ",\"duration_ns\":" << json_uint(static_cast<std::uint64_t>(line.get_number("duration_ns")))
        << ",\"ops_per_second\":" << line.get_number("ops_per_second") << ",\"latency_ns\":{";
    const auto* latency = line.find("latency_ns");
    oss << "\"p50\":" << (latency ? latency->get_number("p50") : 0.0)
        << ",\"p95\":" << (latency ? latency->get_number("p95") : 0.0)
        << ",\"p99\":" << (latency ? latency->get_number("p99") : 0.0)
        << ",\"p999\":" << (latency ? latency->get_number("p999") : 0.0) << "}"
        << ",\"bytes_per_object\":"
        << json_uint(static_cast<std::uint64_t>(line.get_number("bytes_per_object")))
        << ",\"db_bytes\":" << json_uint(static_cast<std::uint64_t>(line.get_number("db_bytes")))
        << ",\"wal_bytes\":" << json_uint(static_cast<std::uint64_t>(line.get_number("wal_bytes")))
        << ",\"peak_rss_bytes\":"
        << json_uint(static_cast<std::uint64_t>(line.get_number("peak_rss_bytes")))
        << ",\"pages_read\":" << json_uint(static_cast<std::uint64_t>(line.get_number("pages_read")))
        << ",\"pages_written\":"
        << json_uint(static_cast<std::uint64_t>(line.get_number("pages_written_estimated")))
        << ",\"pages_reused\":null"
        << ",\"errors\":" << json_uint(static_cast<std::uint64_t>(line.get_number("errors")))
        << ",\"cache_hit_rate\":" << line.get_number("cache_hit_rate", -1.0)
        << ",\"retained_versions\":"
        << json_uint(static_cast<std::uint64_t>(line.get_number("retained_versions"))) << "}";
    return oss.str();
}

} // namespace

RollupExtractResult extract_rollups(const std::filesystem::path& campaign_path,
                                    const std::filesystem::path& environments_file) {
    RollupExtractResult result;

    std::ifstream file(campaign_path, std::ios::binary);
    if (!file) {
        result.error = "não foi possível abrir " + campaign_path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();

    EnvironmentCatalog catalog;
    if (std::filesystem::exists(environments_file)) {
        auto loaded = load_environment_catalog(environments_file);
        if (loaded.ok) {
            catalog = std::move(loaded.catalog);
        }
        // Catálogo malformado não impede a extração -- host_class só fica
        // vazio, e é o indexador quem recusa rollup incompleto (§13.3).
    }

    std::string run_id, started_at;
    std::string git_commit, git_commit_short, git_branch, compiler_id, compiler_version;
    std::string build_type, arch, hostname_token, seed_str;
    std::string instrumentation, os_name, os_version, cpu_model, fs;
    bool git_dirty = false;
    std::uint64_t page_size = 0, format_version = 0, protocol_version = 0;
    std::uint64_t cores_physical = 0, cores_logical = 0, ram_bytes = 0;

    // Chave = (case_id, posição da ocorrência) -- `--repeat N` produz N
    // registros com o MESMO case_id (repeat_index não participa de
    // Case::case_id(), §5), e cada ocorrência precisa virar seu próprio ponto
    // histórico (dedup documentado é run_id+case_id+repeat_index, docs/
    // PLANO_TESTES_DE_CARGA.md §13.9-ish). `run_case_and_record` processa um
    // caso inteiro (case_start...case_summary/case_error) antes do próximo,
    // então as ocorrências de um mesmo case_id nunca se interleavam -- um
    // contador sequencial de "case_start visto" identifica cada uma sem
    // ambiguidade.
    using CaseKey = std::pair<std::string, std::uint64_t>;
    std::map<CaseKey, CaseAccumulator> cases;
    std::vector<CaseKey> case_order;
    CaseKey current_key;
    bool have_current = false;

    std::size_t line_no = 0;
    std::istringstream lines(content);
    std::string raw_line;
    while (std::getline(lines, raw_line)) {
        ++line_no;
        if (raw_line.empty()) {
            continue;
        }
        auto parsed = parse_json(raw_line);
        if (!parsed.ok || !parsed.value.is_object()) {
            result.error = campaign_path.string() + ":" + std::to_string(line_no) +
                           ": linha não é um objeto JSON válido (" + parsed.error + ")";
            return result;
        }
        const auto& v = parsed.value;
        const auto record = v.get_string("record");

        if (record == "run_start") {
            run_id = v.get_string("run_id");
            started_at = v.get_string("started_at");
            seed_str = v.get_string("seed");
        } else if (record == "environment") {
            git_commit = v.get_string("git_commit");
            git_commit_short = v.get_string("git_commit_short");
            git_branch = v.get_string("git_branch");
            git_dirty = v.get_bool("git_dirty");
            compiler_id = v.get_string("compiler_id");
            compiler_version = v.get_string("compiler_version");
            build_type = v.get_string("build_type");
            instrumentation = v.get_string("instrumentation");
            arch = v.get_string("arch");
            os_name = v.get_string("os_name");
            os_version = v.get_string("os_version");
            cpu_model = v.get_string("cpu_model");
            fs = v.get_string("fs");
            cores_physical = static_cast<std::uint64_t>(v.get_number("cores_physical"));
            cores_logical = static_cast<std::uint64_t>(v.get_number("cores_logical"));
            ram_bytes = static_cast<std::uint64_t>(v.get_number("ram_bytes"));
            hostname_token = v.get_string("hostname_token");
            page_size = static_cast<std::uint64_t>(v.get_number("page_size"));
            format_version = static_cast<std::uint64_t>(v.get_number("format_version"));
            protocol_version = static_cast<std::uint64_t>(v.get_number("protocol_version"));
        } else if (record == "case_start") {
            const auto case_id = v.get_string("case_id");
            const auto repeat_index = static_cast<std::uint64_t>(v.get_number("repeat_index"));
            current_key = CaseKey{case_id, repeat_index};
            have_current = true;
            if (!cases.contains(current_key)) {
                case_order.push_back(current_key);
            }
            auto& acc = cases[current_key];
            acc.workload = v.get_string("workload");
            acc.target = v.get_string("target");
            acc.scale = v.get_string("scale");
            acc.objects = static_cast<std::uint64_t>(v.get_number("objects"));
            acc.environment = v.get_string("environment");
            acc.payload = v.get_string("payload");
            acc.batch = static_cast<std::uint64_t>(v.get_number("batch"));
            acc.concurrency = static_cast<std::uint64_t>(v.get_number("concurrency"));
            acc.readers = static_cast<std::uint64_t>(v.get_number("readers"));
            acc.durability = v.get_string("durability");
            acc.cache = v.get_string("cache");
            acc.primary_storage = v.get_string("primary_storage");
            acc.repeat_index = repeat_index;
        } else if (record == "phase_summary") {
            if (!have_current) {
                continue;
            }
            auto& acc = cases[current_key];
            acc.phase_json_objects.push_back(phase_summary_object(v));
            const auto phase_rss = static_cast<std::uint64_t>(v.get_number("peak_rss_bytes"));
            acc.peak_rss_bytes_across_phases = std::max(acc.peak_rss_bytes_across_phases, phase_rss);
        } else if (record == "case_summary") {
            if (!have_current) {
                continue;
            }
            auto& acc = cases[current_key];
            acc.has_summary = true;
            acc.status = v.get_string("status");
            acc.total_duration_ns = v.get_number("total_duration_ns");
            acc.peak_disk_bytes = v.get_number("peak_disk_bytes");
            acc.expected_hash = v.get_string("expected_hash");
            acc.actual_hash = v.get_string("actual_hash");
            acc.hash_match = v.get_bool("hash_match");
            acc.write_amplification = v.get_number("write_amplification");
            acc.space_amplification = v.get_number("space_amplification");
            acc.reclaimed_bytes = v.get_number("reclaimed_bytes");
            acc.db_path = v.get_string("db_path");
            if (const auto* windows = v.find("windows"); windows && windows->is_object()) {
                acc.has_windows = true;
                acc.windows_first_ops_per_second = windows->get_number("first_ops_per_second");
                acc.windows_last_ops_per_second = windows->get_number("last_ops_per_second");
                acc.windows_slope_ops_per_second_per_min =
                    windows->get_number("slope_ops_per_second_per_min");
                acc.windows_first_p99_ns = windows->get_number("first_p99_ns");
                acc.windows_last_p99_ns = windows->get_number("last_p99_ns");
            }
        } else if (record == "case_error") {
            if (!have_current) {
                continue;
            }
            auto& acc = cases[current_key];
            acc.error_message = v.get_string("error");
            if (!acc.has_summary) {
                acc.has_error_only = true;
            }
        }
    }

    const auto raw_sha256 = modb::bench::sha256_hex(modb::bench::sha256_text(content));
    const auto raw_file = campaign_path.filename().string();

    // `os` do rollup é a junção legível de os_name/os_version da campanha, que
    // são dois campos separados no record `environment`.
    const std::string os_display = [&] {
        if (os_name.empty()) {
            return std::string{};
        }
        return os_version.empty() ? os_name : os_name + " " + os_version;
    }();
    // O schema documenta `sanitizers`; `instrumentation` é mais amplo (gprof,
    // coverage). Os dois são emitidos: o campo antigo continua respondendo o que
    // o dashboard já pergunta, o novo carrega o resto.
    const std::string sanitizers_field = [&] {
        if (instrumentation.empty()) {
            return std::string{};
        }
        return instrumentation.find("sanitizers") != std::string::npos ? std::string{"on"}
                                                                      : std::string{"none"};
    }();

    for (const auto& key : case_order) {
        const auto& acc = cases.at(key);
        const auto& case_id = key.first;

        // Workload/target sem dispatch implementado: um case_error citando
        // isso não é uma medição -- não vira ponto histórico (docs-process/
        // PLANO_IMPLEMENTACAO_CARGA.md, Subfase C).
        if (acc.has_error_only && is_unimplemented_error_text(acc.error_message)) {
            continue;
        }
        if (!acc.has_summary && !acc.has_error_only) {
            continue; // case_start sem summary nem error: campanha interrompida no meio do caso
        }

        const std::string host_class = [&] {
            const auto* entry = catalog.find(acc.environment);
            return entry ? entry->host_class : std::string{};
        }();

        SeriesKeyInput key_input;
        key_input.case_id = case_id;
        key_input.workload_version = 1;   // sem versionamento de workload ainda
        key_input.dataset_id = "user_v1"; // único dataset existente (Subfase B)
        key_input.dataset_version = 1;
        key_input.scale = acc.scale;
        key_input.payload = acc.payload;
        key_input.batch = acc.batch;
        key_input.concurrency = acc.concurrency;
        key_input.durability = acc.durability;
        key_input.cache = acc.cache;
        key_input.primary_storage = acc.primary_storage;
        key_input.build_type = build_type;
        key_input.instrumentation = instrumentation;
        key_input.arch = arch;
        key_input.page_size = page_size;
        key_input.format_version = format_version;
        key_input.protocol_version = protocol_version;
        key_input.host_class = host_class;
        key_input.target = acc.target;
        const auto series_key = compute_series_key(key_input);

        const std::string status = acc.has_summary ? acc.status : "failed";

        std::ostringstream oss;
        oss << "{\"schema\":\"modb.loadtest.rollup\",\"schema_version\":1"
            << ",\"series_key\":" << json_string(series_key)
            << ",\"series_key_version\":" << json_uint(series_key_version)
            << ",\"case_id\":" << json_string(case_id) << ",\"workload\":" << json_string(acc.workload)
            << ",\"target\":" << json_string(acc.target) << ",\"scale\":" << json_string(acc.scale)
            << ",\"objects\":" << json_uint(acc.objects) << ",\"variant\":" << json_string("")
            << ",\"environment\":" << json_string(acc.environment)
            << ",\"run_id\":" << json_string(run_id) << ",\"started_at\":" << json_string(started_at)
            << ",\"repeat_index\":" << json_uint(acc.repeat_index)
            << ",\"commit\":" << json_string(git_commit)
            << ",\"commit_short\":" << json_string(git_commit_short)
            << ",\"branch\":" << json_string(git_branch) << ",\"tree_dirty\":" << json_bool(git_dirty)
            << ",\"diff_hash\":null"
            << ",\"workload_version\":" << json_uint(key_input.workload_version)
            << ",\"dataset_id\":" << json_string(key_input.dataset_id)
            << ",\"dataset_version\":" << json_uint(key_input.dataset_version)
            << ",\"seed\":" << json_string(seed_str) << ",\"env\":{"
            << "\"host_id\":" << json_string(hostname_token)
            << ",\"host_class\":" << json_string(host_class)
            << ",\"os\":" << string_or_null(os_display) << ",\"arch\":" << json_string(arch)
            << ",\"cpu_model\":" << string_or_null(cpu_model)
            << ",\"cores_physical\":" << uint_or_null(cores_physical)
            << ",\"cores_logical\":" << uint_or_null(cores_logical)
            << ",\"ram_gb\":" << ram_gb_or_null(ram_bytes)
            << ",\"fs\":" << string_or_null(fs)
            // device_class (nvme/ssd/hdd) exige IOCTL_STORAGE_QUERY_PROPERTY por
            // volume; permanece não coletado em vez de adivinhado
            // (docs-process/PLANO_PROFILING.md §8).
            << ",\"device_class\":null"
            << ",\"build_type\":" << json_string(build_type) << ",\"compiler\":"
            << json_string(compiler_id + " " + compiler_version)
            << ",\"sanitizers\":" << string_or_null(sanitizers_field)
            << ",\"instrumentation\":" << string_or_null(instrumentation)
            << ",\"page_size\":" << json_uint(page_size)
            << ",\"format_version\":" << json_uint(format_version)
            << ",\"protocol_version\":" << json_uint(protocol_version) << "}"
            << ",\"params\":{\"payload\":" << json_string(acc.payload)
            << ",\"batch\":" << json_uint(acc.batch) << ",\"concurrency\":" << json_uint(acc.concurrency)
            << ",\"readers\":" << json_uint(acc.readers) << ",\"durability\":"
            << json_string(acc.durability) << ",\"cache\":" << json_string(acc.cache)
            << ",\"primary_storage\":" << json_string(acc.primary_storage) << "}"
            << ",\"phases\":[";
        for (std::size_t i = 0; i < acc.phase_json_objects.size(); ++i) {
            if (i > 0) {
                oss << ',';
            }
            oss << acc.phase_json_objects[i];
        }
        oss << "]"
            << ",\"totals\":{\"duration_ns\":" << json_uint(static_cast<std::uint64_t>(acc.total_duration_ns))
            << ",\"peak_disk_bytes\":" << json_uint(static_cast<std::uint64_t>(acc.peak_disk_bytes))
            << ",\"peak_rss_bytes\":" << json_uint(acc.peak_rss_bytes_across_phases)
            << ",\"reclaimed_bytes\":" << json_uint(static_cast<std::uint64_t>(acc.reclaimed_bytes))
            << ",\"write_amplification\":" << acc.write_amplification
            << ",\"space_amplification\":" << acc.space_amplification << "}"
            << ",\"windows\":";
        if (acc.has_windows) {
            oss << "{\"first_ops_per_second\":" << acc.windows_first_ops_per_second
                << ",\"last_ops_per_second\":" << acc.windows_last_ops_per_second
                << ",\"slope_ops_per_second_per_min\":" << acc.windows_slope_ops_per_second_per_min
                << ",\"first_p99_ns\":" << acc.windows_first_p99_ns
                << ",\"last_p99_ns\":" << acc.windows_last_p99_ns << "}";
        } else {
            oss << "null";
        }
        oss << ",\"status\":" << json_string(status)
            << ",\"comparable\":" << json_bool(true) << ",\"validations\":"
            << (acc.has_summary ? "[\"count\",\"logical_hash\"]" : "[]")
            << ",\"raw_file\":" << json_string(raw_file)
            << ",\"raw_sha256\":" << json_string(raw_sha256)
            << (acc.has_summary ? "" : ",\"error\":" + json_string(acc.error_message)) << "}";
        result.rollup_lines.push_back(oss.str());
    }

    result.ok = true;
    return result;
}

} // namespace modb::loadtest
