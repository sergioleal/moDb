#include "runner/campaign.hpp"

#include "runner/environment.hpp"
#include "runner/json_util.hpp"
#include "runner/jsonl_writer.hpp"
#include "runner/profile.hpp"
#include "scenarios/buffer_pool_oversubscribed.hpp"
#include "scenarios/graph_traversal.hpp"
#include "scenarios/object_store_lifecycle.hpp"
#include "scenarios/object_store_read_hotpath.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace modb::bench {
namespace {

bool match_filter(std::string_view scenario_id, std::string_view filter) {
    if (filter.empty()) {
        return true;
    }
    return scenario_id.find(filter) != std::string_view::npos;
}

std::string parameters_json(const ScenarioProfileOverride& scenario) {
    std::ostringstream oss;
    oss << "{\"object_count\":" << scenario.object_count << ",\"stride\":" << scenario.stride;
    if (scenario.cache_pages > 0) {
        oss << ",\"cache_pages\":" << scenario.cache_pages;
    }
    if (scenario.read_rounds > 0) {
        oss << ",\"read_rounds\":" << scenario.read_rounds;
    }
    oss << "}";
    return oss.str();
}

std::string parameters_key(const ScenarioProfileOverride& scenario) {
    auto key = scenario.scenario_id + "|count=" + std::to_string(scenario.object_count) +
               "|stride=" + std::to_string(scenario.stride);
    if (scenario.cache_pages > 0) {
        key += "|cache=" + std::to_string(scenario.cache_pages);
    }
    if (scenario.read_rounds > 0) {
        key += "|rounds=" + std::to_string(scenario.read_rounds);
    }
    return key;
}

std::string sample_json(std::string_view run_id, std::uint64_t sequence,
                        std::string_view scenario_id, int iteration, const SampleResult& sample) {
    std::ostringstream oss;
    oss << "{\"schema\":\"modb.benchmark\",\"schema_version\":1,\"record\":\"sample\",\"run_id\":"
        << json_string(run_id) << ",\"sequence\":" << sequence
        << ",\"scenario_id\":" << json_string(scenario_id) << ",\"iteration\":" << iteration
        << ",\"valid\":" << json_bool(sample.valid)
        << ",\"comparable\":" << json_bool(sample.comparable) << ",\"metrics\":{"
        << "\"operations\":" << sample.metrics.operations
        << ",\"elapsed_ns\":" << sample.metrics.elapsed_ns
        << ",\"create_ns\":" << sample.metrics.create_ns
        << ",\"create_flush_ns\":" << sample.metrics.create_flush_ns
        << ",\"delete_ns\":" << sample.metrics.delete_ns
        << ",\"delete_flush_ns\":" << sample.metrics.delete_flush_ns
        << ",\"peak_file_bytes\":" << sample.metrics.peak_file_bytes
        << ",\"objects_per_second\":" << json_number(sample.metrics.objects_per_second) << "}";
    if (!sample.error.empty()) {
        oss << ",\"error\":" << json_string(sample.error);
    }
    oss << "}";
    return oss.str();
}

bool extract_json_string_field(std::string_view line, std::string_view key, std::string& out) {
    const std::string needle = "\"" + std::string{key} + "\":\"";
    const auto pos = line.find(needle);
    if (pos == std::string_view::npos) {
        return false;
    }
    auto start = pos + needle.size();
    std::string value;
    for (std::size_t i = start; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            value.push_back(line[i + 1]);
            ++i;
            continue;
        }
        if (line[i] == '"') {
            out = value;
            return true;
        }
        value.push_back(line[i]);
    }
    return false;
}

bool extract_json_number_field(std::string_view line, std::string_view key, double& out) {
    const std::string needle = "\"" + std::string{key} + "\":";
    const auto pos = line.find(needle);
    if (pos == std::string_view::npos) {
        return false;
    }
    auto start = pos + needle.size();
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    std::size_t end = start;
    while (end < line.size() &&
           (std::isdigit(static_cast<unsigned char>(line[end])) || line[end] == '.' ||
            line[end] == '-' || line[end] == '+' || line[end] == 'e' || line[end] == 'E')) {
        ++end;
    }
    if (end == start) {
        return false;
    }
    try {
        out = std::stod(std::string{line.substr(start, end - start)});
        return true;
    } catch (...) {
        return false;
    }
}

struct SummaryRow {
    std::string scenario_id;
    std::string parameters;
    std::string parameters_key;
    double operations_per_second{0.0};
    int valid_samples{0};
};

std::vector<SummaryRow> load_summaries(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "nao foi possivel abrir " + path.string();
        return {};
    }
    std::vector<SummaryRow> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("\"record\":\"scenario_summary\"") == std::string::npos) {
            continue;
        }
        SummaryRow row;
        if (!extract_json_string_field(line, "scenario_id", row.scenario_id)) {
            continue;
        }
        extract_json_string_field(line, "parameters_key", row.parameters_key);
        extract_json_string_field(line, "parameters", row.parameters);
        double ops = 0.0;
        double valid = 0.0;
        extract_json_number_field(line, "operations_per_second", ops);
        extract_json_number_field(line, "valid_samples", valid);
        row.operations_per_second = ops;
        row.valid_samples = static_cast<int>(valid);
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

CampaignResult run_campaign(const CampaignOptions& options) {
    CampaignResult result;
    auto profile = find_profile(options.profile);
    if (!profile) {
        result.error = "perfil desconhecido: " + options.profile;
        result.status = "failed";
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(options.output_dir, ec);
    if (ec) {
        result.error = "falha ao criar output-dir: " + ec.message();
        result.status = "failed";
        return result;
    }

    auto work_dir = options.work_dir;
    if (work_dir.empty()) {
        work_dir = options.output_dir / "work";
    }
    std::filesystem::create_directories(work_dir, ec);

    const auto env = collect_environment(options.argv_joined);
    const auto stamp = utc_timestamp_millis();
    const auto run_id = make_run_id(stamp);
    const auto filename =
        make_result_filename(stamp, env.git_commit_short, env.hostname_token);
    const auto final_path = options.output_dir / filename;

    JsonlWriter writer;
    if (!writer.open(final_path)) {
        result.error = "nao foi possivel abrir resultado (existe ou permissao): " +
                       final_path.string();
        result.status = "failed";
        return result;
    }

    result.run_id = run_id;
    result.result_path = final_path;

    auto write_or_fail = [&](const std::string& line) -> bool {
        if (!writer.write_line(line)) {
            result.error = "falha ao gravar JSONL";
            result.status = "failed";
            writer.abandon();
            return false;
        }
        return true;
    };

    {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.benchmark\",\"schema_version\":1,\"record\":\"run_start\","
               "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
            << ",\"started_at\":" << json_string(stamp)
            << ",\"profile\":" << json_string(profile->name)
            << ",\"seed\":" << json_string(std::to_string(options.seed))
            << ",\"command\":" << json_string(options.argv_joined) << "}";
        if (!write_or_fail(oss.str())) {
            return result;
        }
    }

    {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.benchmark\",\"schema_version\":1,\"record\":\"environment\","
               "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
            << ",\"git\":{\"commit\":" << json_string(env.git_commit)
            << ",\"branch\":" << json_string(env.git_branch)
            << ",\"dirty\":" << json_bool(env.git_dirty) << "},\"compiler\":{\"id\":"
            << json_string(env.compiler_id) << ",\"version\":" << json_string(env.compiler_version)
            << "},\"cxx_standard\":" << json_string(env.cxx_standard)
            << ",\"build_type\":" << json_string(env.build_type)
            << ",\"os\":{\"name\":" << json_string(env.os_name)
            << ",\"version\":" << json_string(env.os_version) << "},\"arch\":"
            << json_string(env.arch) << ",\"hostname_token\":" << json_string(env.hostname_token)
            << ",\"page_size\":" << env.page_size
            << ",\"project_version\":" << json_string(env.project_version)
            << ",\"argv\":" << json_string(env.argv_joined) << "}";
        if (!write_or_fail(oss.str())) {
            return result;
        }
    }

    int scenarios_ok = 0;
    int scenarios_failed = 0;
    int samples_written = 0;
    bool campaign_failed = false;

    for (const auto& scenario : profile->scenarios) {
        if (!match_filter(scenario.scenario_id, options.filter)) {
            continue;
        }
        if (scenario.scenario_id != "object_store.lifecycle" &&
            scenario.scenario_id != "storage.buffer_pool.oversubscribed" &&
            scenario.scenario_id != "object_store.read_hotpath") {
            std::ostringstream note;
            note << "{\"schema\":\"modb.benchmark\",\"schema_version\":1,\"record\":\"run_note\","
                    "\"run_id\":"
                 << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
                 << ",\"note\":" << json_string("cenario ainda nao implementado: " +
                                               scenario.scenario_id)
                 << "}";
            if (!write_or_fail(note.str())) {
                return result;
            }
            continue;
        }

        const auto params_json = parameters_json(scenario);
        const auto key = parameters_key(scenario);
        {
            std::ostringstream oss;
            oss << "{\"schema\":\"modb.benchmark\",\"schema_version\":1,\"record\":\"scenario_"
                   "start\",\"run_id\":"
                << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
                << ",\"scenario_id\":" << json_string(scenario.scenario_id)
                << ",\"parameters\":" << params_json
                << ",\"parameters_key\":" << json_string(key)
                << ",\"cache_state\":"
                << json_string(scenario.scenario_id == "storage.buffer_pool.oversubscribed"
                                   ? "oversubscribed"
                                   : "warm")
                << ",\"dataset\":{\"id\":\"synthetic.item.v1\",\"seed\":" << options.seed << "}}";
            if (!write_or_fail(oss.str())) {
                return result;
            }
        }

        std::cerr << "cenario " << scenario.scenario_id << " count=" << scenario.object_count;
        if (scenario.cache_pages > 0) {
            std::cerr << " cache_pages=" << scenario.cache_pages;
        }
        std::cerr << " stride=" << scenario.stride << '\n';

        const auto run_one = [&]() -> SampleResult {
            if (scenario.scenario_id == "storage.buffer_pool.oversubscribed") {
                BufferPoolParams params;
                params.seed = options.seed;
                params.object_count = scenario.object_count;
                params.cache_pages = scenario.cache_pages == 0 ? 8 : scenario.cache_pages;
                params.work_dir = work_dir.string();
                return run_buffer_pool_oversubscribed(params);
            }
            if (scenario.scenario_id == "object_store.read_hotpath") {
                ReadHotpathParams params;
                params.seed = options.seed;
                params.object_count = scenario.object_count;
                params.read_rounds = scenario.read_rounds == 0 ? 1 : scenario.read_rounds;
                params.work_dir = work_dir.string();
                return run_object_store_read_hotpath(params);
            }
            if (scenario.scenario_id == "graph.traversal.warm" ||
                scenario.scenario_id == "graph.traversal.cold") {
                GraphTraversalParams params;
                params.scenario_id = scenario.scenario_id;
                params.seed = options.seed;
                params.branching =
                    static_cast<std::uint32_t>(scenario.stride == 0 ? 2 : scenario.stride);
                params.depth = static_cast<std::uint32_t>(
                    scenario.object_count == 0 ? 3 : scenario.object_count);
                params.cache_state =
                    scenario.scenario_id == "graph.traversal.cold" ? "cold" : "warm";
                params.work_dir = work_dir.string();
                return run_graph_traversal(params);
            }
            ScenarioParams params;
            params.scenario_id = scenario.scenario_id;
            params.seed = options.seed;
            params.object_count = scenario.object_count;
            params.stride = scenario.stride;
            params.work_dir = work_dir.string();
            return run_object_store_lifecycle(params);
        };

        std::vector<double> elapsed_samples;
        std::vector<double> throughput_samples;
        int valid_samples = 0;
        std::string scenario_error;

        const int warmup = scenario.warmup > 0 ? scenario.warmup : profile->default_warmup;
        const int samples = scenario.samples > 0 ? scenario.samples : profile->default_samples;

        for (int i = 0; i < warmup; ++i) {
            auto warm = run_one();
            if (!warm.valid) {
                scenario_error = warm.error;
                campaign_failed = true;
                break;
            }
        }
        if (campaign_failed) {
            std::ostringstream err;
            err << "{\"schema\":\"modb.benchmark\",\"schema_version\":1,\"record\":\"scenario_"
                   "error\",\"run_id\":"
                << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
                << ",\"scenario_id\":" << json_string(scenario.scenario_id)
                << ",\"error\":" << json_string(scenario_error) << "}";
            write_or_fail(err.str());
            ++scenarios_failed;
            break;
        }

        for (int iteration = 1; iteration <= samples; ++iteration) {
            auto sample = run_one();
            if (!write_or_fail(sample_json(run_id, writer.next_sequence(), scenario.scenario_id,
                                           iteration, sample))) {
                return result;
            }
            ++samples_written;
            if (!sample.valid) {
                scenario_error = sample.error;
                campaign_failed = true;
                break;
            }
            ++valid_samples;
            elapsed_samples.push_back(static_cast<double>(sample.metrics.elapsed_ns));
            throughput_samples.push_back(sample.metrics.objects_per_second);
        }

        if (campaign_failed) {
            std::ostringstream err;
            err << "{\"schema\":\"modb.benchmark\",\"schema_version\":1,\"record\":\"scenario_"
                   "error\",\"run_id\":"
                << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
                << ",\"scenario_id\":" << json_string(scenario.scenario_id)
                << ",\"error\":" << json_string(scenario_error) << "}";
            write_or_fail(err.str());
            ++scenarios_failed;
            break;
        }

        const auto latency = summarize_latency_ns(elapsed_samples);
        double mean_ops = 0.0;
        if (!throughput_samples.empty()) {
            for (const double value : throughput_samples) {
                mean_ops += value;
            }
            mean_ops /= static_cast<double>(throughput_samples.size());
        }

        {
            std::ostringstream oss;
            oss << "{\"schema\":\"modb.benchmark\",\"schema_version\":1,\"record\":\"scenario_"
                   "summary\",\"run_id\":"
                << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
                << ",\"scenario_id\":" << json_string(scenario.scenario_id)
                << ",\"parameters\":" << params_json
                << ",\"parameters_key\":" << json_string(key)
                << ",\"warmup_iterations\":" << warmup << ",\"valid_samples\":" << valid_samples
                << ",\"latency_ns\":{\"min\":" << json_number(latency.min)
                << ",\"p50\":" << json_number(latency.p50) << ",\"p90\":" << json_number(latency.p90)
                << ",\"p95\":" << json_number(latency.p95) << ",\"p99\":" << json_number(latency.p99)
                << ",\"max\":" << json_number(latency.max) << ",\"mean\":" << json_number(latency.mean)
                << "},\"operations_per_second\":" << json_number(mean_ops) << "}";
            if (!write_or_fail(oss.str())) {
                return result;
            }
        }
        ++scenarios_ok;
        std::cerr << "  ops/s mediana~ " << mean_ops << '\n';
    }

    const std::string status =
        campaign_failed ? "failed" : (scenarios_ok > 0 ? "completed" : "partial");
    const auto content_hash = writer.content_sha256_hex();
    {
        std::ostringstream oss;
        oss << "{\"schema\":\"modb.benchmark\",\"schema_version\":1,\"record\":\"run_end\","
               "\"run_id\":"
            << json_string(run_id) << ",\"sequence\":" << writer.next_sequence()
            << ",\"status\":" << json_string(status) << ",\"scenarios_ok\":" << scenarios_ok
            << ",\"scenarios_failed\":" << scenarios_failed
            << ",\"samples_written\":" << samples_written
            << ",\"content_sha256\":" << json_string(content_hash) << "}";
        if (!write_or_fail(oss.str())) {
            return result;
        }
    }

    if (!writer.finish()) {
        result.error = "falha ao promover .partial para o nome final";
        result.status = "failed";
        return result;
    }

    result.ok = !campaign_failed && scenarios_ok > 0;
    result.status = status;
    std::cerr << "resultado: " << final_path.string() << '\n';
    std::cerr << "run_id: " << run_id << " status=" << status << '\n';
    std::cerr << "content_sha256: " << content_hash << '\n';
    return result;
}

CompareResult compare_campaigns(const std::filesystem::path& baseline,
                                const std::filesystem::path& candidate) {
    CompareResult result;
    std::string error;
    const auto base_rows = load_summaries(baseline, error);
    if (!error.empty()) {
        result.error = error;
        return result;
    }
    error.clear();
    const auto cand_rows = load_summaries(candidate, error);
    if (!error.empty()) {
        result.error = error;
        return result;
    }

    std::unordered_map<std::string, SummaryRow> base_map;
    for (const auto& row : base_rows) {
        const auto key = row.parameters_key.empty()
                             ? row.scenario_id + "|" + row.parameters
                             : row.parameters_key;
        base_map[key] = row;
    }

    for (const auto& cand : cand_rows) {
        CompareScenarioDelta delta;
        delta.scenario_id = cand.scenario_id;
        delta.parameters_key =
            cand.parameters_key.empty() ? cand.scenario_id + "|" + cand.parameters
                                       : cand.parameters_key;
        delta.candidate_ops_per_sec = cand.operations_per_second;
        const auto it = base_map.find(delta.parameters_key);
        if (it == base_map.end()) {
            delta.compatible = false;
            delta.verdict = "incompatible";
            result.deltas.push_back(delta);
            continue;
        }
        delta.compatible = true;
        delta.baseline_ops_per_sec = it->second.operations_per_second;
        if (delta.baseline_ops_per_sec <= 0.0) {
            delta.delta_percent = 0.0;
            delta.verdict = "ok";
        } else {
            delta.delta_percent =
                ((delta.candidate_ops_per_sec - delta.baseline_ops_per_sec) /
                 delta.baseline_ops_per_sec) *
                100.0;
            if (delta.delta_percent <= -10.0) {
                delta.verdict = "fail";
            } else if (delta.delta_percent <= -5.0) {
                delta.verdict = "alert";
            } else {
                delta.verdict = "ok";
            }
        }
        result.deltas.push_back(delta);
    }

    result.ok = true;
    return result;
}

} // namespace modb::bench
