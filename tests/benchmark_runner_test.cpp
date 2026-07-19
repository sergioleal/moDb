#include "runner/campaign.hpp"
#include "runner/json_util.hpp"
#include "runner/jsonl_writer.hpp"
#include "runner/sha256.hpp"
#include "scenarios/object_store_lifecycle.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FALHA: " << message << '\n';
        ++g_failures;
    }
}

} // namespace

int main() {
    using namespace modb::bench;

    expect(json_string("a\"b") == "\"a\\\"b\"", "json_string escapa aspas");
    expect(json_bool(true) == "true", "json_bool true");

    const auto empty_hash = sha256_hex(sha256_text(""));
    expect(empty_hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "sha256(\"\")");

    std::vector<double> samples{100, 200, 300, 400, 500};
    const auto stats = summarize_latency_ns(samples);
    expect(stats.min == 100.0, "latency min");
    expect(stats.max == 500.0, "latency max");
    expect(stats.p50 == 300.0, "latency p50");

    const auto tmp = std::filesystem::temp_directory_path() /
                     ("modb-bench-test-" + std::to_string(
                                              std::chrono::steady_clock::now()
                                                  .time_since_epoch()
                                                  .count()));
    std::filesystem::create_directories(tmp);
    const auto final_path = tmp / "sample.jsonl";

    {
        JsonlWriter writer;
        expect(writer.open(final_path), "jsonl open");
        expect(writer.write_line(
                   R"({"schema":"modb.benchmark","schema_version":1,"record":"run_start"})"),
               "jsonl write");
        expect(writer.finish(), "jsonl finish");
        expect(std::filesystem::exists(final_path), "jsonl final existe");
        expect(!std::filesystem::exists(final_path.string() + ".partial"),
               "partial removido apos finish");
    }

    ScenarioParams params;
    params.scenario_id = "object_store.lifecycle";
    params.seed = 42;
    params.object_count = 40;
    params.stride = 1;
    params.work_dir = tmp.string();
    const auto sample = run_object_store_lifecycle(params);
    expect(sample.valid, "lifecycle sample valido");
    expect(sample.metrics.operations == 80, "lifecycle operations");
    expect(sample.metrics.elapsed_ns > 0, "lifecycle elapsed");

    CampaignOptions options;
    options.profile = "diagnostic";
    options.seed = 7;
    options.output_dir = tmp / "out1";
    options.work_dir = tmp / "work1";
    options.argv_joined = "modb_bench run --profile diagnostic --seed 7";
    const auto camp1 = run_campaign(options);
    expect(camp1.ok, "campanha diagnostic 1");
    expect(std::filesystem::exists(camp1.result_path), "jsonl campanha 1");

    options.output_dir = tmp / "out2";
    options.work_dir = tmp / "work2";
    const auto camp2 = run_campaign(options);
    expect(camp2.ok, "campanha diagnostic 2");

    const auto compared = compare_campaigns(camp1.result_path, camp2.result_path);
    expect(compared.ok, "compare ok");
    expect(!compared.deltas.empty(), "compare tem deltas");
    expect(compared.deltas.front().compatible, "cenarios compativeis");
    expect(compared.deltas.front().verdict == "ok" || compared.deltas.front().verdict == "alert",
           "sem regressao sintetica forte entre duas campanhas iguais");

    std::ifstream in(camp1.result_path);
    std::string line;
    bool saw_env = false;
    bool saw_end = false;
    while (std::getline(in, line)) {
        if (line.find("\"record\":\"environment\"") != std::string::npos) {
            saw_env = true;
        }
        if (line.find("\"record\":\"run_end\"") != std::string::npos) {
            saw_end = true;
        }
    }
    expect(saw_env, "JSONL contem environment");
    expect(saw_end, "JSONL contem run_end");

    std::error_code ignored;
    std::filesystem::remove_all(tmp, ignored);

    if (g_failures != 0) {
        std::cerr << g_failures << " falha(s)\n";
        return 1;
    }
    std::cout << "modb.benchmark_runner: ok\n";
    return 0;
}
