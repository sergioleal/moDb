#include "test_support.hpp"

#include "history/baselines.hpp"
#include "history/gate.hpp"
#include "history/index.hpp"
#include "history/retention.hpp"
#include "history/series_key.hpp"
#include "history/trend.hpp"
#include "json_value.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

using namespace modb::loadtest;

namespace {

std::filesystem::path make_temp_dir() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
              ("modb-load-history-test-" + std::to_string(unique));
    std::filesystem::create_directories(dir);
    return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::string fake_environments_json() {
    return R"({"schema":"modb.loadtest.environments","schema_version":1,"environments":)"
          R"([{"id":"test-env","label":"Test","kind":"local","host_class":"test-host-class"}]})";
}

// Campanha mínima porém válida (schema modb.loadtest, §12): um caso
// `create_only` completo, os mesmos records que campaign.cpp de fato emite.
std::string fake_campaign_jsonl(const std::string& run_id, const std::string& started_at,
                                double ops_per_second, bool include_environment_record = true,
                                const std::string& windows_json = "") {
    std::ostringstream oss;
    oss << R"({"schema":"modb.loadtest","schema_version":1,"record":"run_start",)"
        << R"("run_id":")" << run_id << R"(","sequence":1,"started_at":")" << started_at
        << R"(","profile":"","seed":"42","command":"test"})" << '\n';
    if (include_environment_record) {
        oss << R"({"schema":"modb.loadtest","schema_version":1,"record":"environment",)"
            << R"("run_id":")" << run_id
            << R"(","sequence":2,"git_commit":"abcdef0123456789","git_commit_short":"abcdef0",)"
            << R"("git_branch":"master","git_dirty":false,"compiler_id":"gcc",)"
            << R"("compiler_version":"14","cxx_standard":"26","build_type":"Debug",)"
            << R"("os_name":"Windows","os_version":"11","arch":"x86_64",)"
            << R"("hostname_token":"h1234567","page_size":"4096","project_version":"0.1.0",)"
            << R"("format_version":1,"protocol_version":1})" << '\n';
    }
    oss << R"({"schema":"modb.loadtest","schema_version":1,"record":"case_start",)"
        << R"("run_id":")" << run_id
        << R"(","sequence":3,"case_id":"load.create_only.embedded.10k","workload":"create_only",)"
        << R"("target":"embedded","scale":"10k","objects":10000,"environment":"test-env",)"
        << R"("payload":"normal","batch":1000,"concurrency":1,"readers":0,)"
        << R"("durability":"sync_real","cache":"warm","primary_storage":"full","repeat_index":0})"
        << '\n';
    oss << R"({"schema":"modb.loadtest","schema_version":1,"record":"phase_summary",)"
        << R"("run_id":")" << run_id
        << R"(","sequence":4,"case_id":"load.create_only.embedded.10k","phase":"create",)"
        << R"("operations":10000,"duration_ns":500000000,"ops_per_second":)" << ops_per_second
        << R"(,"errors":0,"latency_ns":{"p50":100,"p95":200,"p99":300,"p999":400},)"
        << R"("peak_rss_bytes":1000,"db_bytes":2000,"wal_bytes":1000,"pages_read":0,)"
        << R"("pages_written_estimated":10})" << '\n';
    oss << R"({"schema":"modb.loadtest","schema_version":1,"record":"case_summary",)"
        << R"("run_id":")" << run_id
        << R"(","sequence":5,"case_id":"load.create_only.embedded.10k","status":"completed",)"
        << R"("total_duration_ns":500000000,"peak_disk_bytes":3000,"expected_hash":"h1",)"
        << R"("actual_hash":"h1","hash_match":true,"write_amplification":1.5,)"
        << R"("space_amplification":1.5,"db_path":"x.modb")";
    if (!windows_json.empty()) {
        oss << R"(,"windows":)" << windows_json;
    }
    oss << "}" << '\n';
    oss << R"({"schema":"modb.loadtest","schema_version":1,"record":"run_end",)"
        << R"("run_id":")" << run_id
        << R"(","sequence":6,"status":"completed","completed":1,"failed":0,"unimplemented":0,)"
        << R"("previous_content_sha256":"x"})" << '\n';
    return oss.str();
}

void test_series_key_stable(TestSuite& suite) {
    SeriesKeyInput input;
    input.case_id = "load.create_only.embedded.10k";
    input.workload_version = 1;
    input.dataset_id = "user_v1";
    input.dataset_version = 1;
    input.scale = "10k";
    input.payload = "normal";
    input.batch = 1000;
    input.concurrency = 1;
    input.durability = "sync_real";
    input.cache = "warm";
    input.primary_storage = "full";
    input.build_type = "Release";
    input.arch = "x86_64";
    input.page_size = 4096;
    input.format_version = 1;
    input.protocol_version = 1;
    input.host_class = "bench-linux-01";
    input.target = "embedded";

    const auto key1 = compute_series_key(input);
    const auto key2 = compute_series_key(input);
    suite.check(key1 == key2, "mesma entrada deve produzir sempre a mesma series_key");
    suite.check(key1.size() == 16, "series_key deve ter 16 hex chars");

    auto changed = input;
    changed.host_class = "dev-windows";
    suite.check(compute_series_key(changed) != key1,
               "host_class diferente deve produzir series_key diferente");

    auto changed_build = input;
    changed_build.build_type = "Debug";
    suite.check(compute_series_key(changed_build) != key1,
               "build_type diferente deve produzir series_key diferente");
}

void test_index_idempotent(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto campaign_path = dir / "campaign.jsonl";
    const auto history_path = dir / "series.jsonl";
    const auto env_path = dir / "environments.json";

    write_file(campaign_path, fake_campaign_jsonl("run-idx-1", "20260101T000000.000Z", 20000));
    write_file(env_path, fake_environments_json());

    auto first = index_campaign(campaign_path, history_path, env_path);
    suite.check(first.ok, "primeira indexação deve funcionar: " + first.error);
    suite.check(first.appended == 1, "primeira indexação deve acrescentar exatamente 1 ponto");
    suite.check(first.rejected == 0, "campanha válida não deve ser rejeitada");

    auto second = index_campaign(campaign_path, history_path, env_path);
    suite.check(second.ok, "segunda indexação (mesma campanha) deve funcionar");
    suite.check(second.appended == 0, "reindexar a mesma campanha não deve acrescentar de novo");
    suite.check(second.skipped_duplicate == 1, "reindexar deve reportar 1 duplicata");

    std::ifstream history_file(history_path);
    std::size_t line_count = 0;
    std::string line;
    while (std::getline(history_file, line)) {
        if (!line.empty()) {
            ++line_count;
        }
    }
    suite.check(line_count == 1, "arquivo histórico deve ter exatamente 1 linha após reindexar");
}

void test_index_rejects_missing_provenance(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto campaign_path = dir / "campaign_no_env_record.jsonl";
    const auto history_path = dir / "series.jsonl";
    const auto env_path = dir / "environments.json";

    // Sem o record "environment": build_type/host_class ficam vazios no rollup.
    write_file(campaign_path,
              fake_campaign_jsonl("run-rej-1", "20260101T000000.000Z", 20000,
                                 /*include_environment_record=*/false));
    write_file(env_path, fake_environments_json());

    auto indexed = index_campaign(campaign_path, history_path, env_path);
    suite.check(indexed.ok, "index_campaign não deve falhar globalmente por um rollup incompleto");
    suite.check(indexed.appended == 0, "rollup sem build_type não deve ser acrescentado");
    suite.check(indexed.rejected == 1, "rollup sem build_type deve ser rejeitado");
    suite.check(!indexed.rejection_reasons.empty(), "motivo da rejeição deve ser reportado");
    suite.check(!std::filesystem::exists(history_path) ||
                   std::ifstream(history_path).peek() == std::ifstream::traits_type::eof(),
               "nenhuma linha deve ter sido gravada");
}

void test_trend_median_and_verdict(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto history_path = dir / "series.jsonl";
    const auto env_path = dir / "environments.json";
    write_file(env_path, fake_environments_json());

    // 5 execuções na mesma série: 100,100,100,100 e uma queda de 50% na 5a.
    std::ostringstream campaigns;
    const std::vector<double> values = {100, 100, 100, 100, 50};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto run_id = "run-trend-" + std::to_string(i);
        const auto started_at = "2026010" + std::to_string(i + 1) + "T000000.000Z";
        const auto campaign_path = dir / (run_id + ".jsonl");
        write_file(campaign_path, fake_campaign_jsonl(run_id, started_at, values[i]));
        auto indexed = index_campaign(campaign_path, history_path, env_path);
        suite.check(indexed.ok && indexed.appended == 1,
                   "cada campanha de teste deve indexar exatamente 1 ponto");
    }

    auto trend = compute_trend(history_path, "load.create_only.embedded.10k", "ops_per_second",
                               "create");
    suite.check(trend.ok, "compute_trend não deve falhar: " + trend.error);
    suite.check(trend.points.size() == 5, "devem existir 5 pontos na série");
    if (trend.points.size() == 5) {
        suite.check(trend.points[0].verdict == "insufficient",
                   "1o ponto: histórico insuficiente (0 anteriores)");
        suite.check(trend.points[1].verdict == "insufficient",
                   "2o ponto: histórico insuficiente (1 anterior)");
        suite.check(trend.points[2].verdict == "insufficient",
                   "3o ponto: histórico insuficiente (2 anteriores)");
        suite.check(trend.points[3].verdict == "ok",
                   "4o ponto: 3 anteriores iguais -> mediana igual -> ok");
        suite.check(trend.points[4].verdict == "fail",
                   "5o ponto: queda de 50% deve reprovar (limiar 10% para ops_per_second)");
    }
}

void test_trend_series_break_resets_window(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto history_path = dir / "series.jsonl";
    const auto env_path = dir / "environments.json";
    write_file(env_path, fake_environments_json());

    // 4 pontos na "série A" (build_type Debug) + 1 ponto na "série B"
    // (troca de build_type -- muda a series_key, §13.4).
    for (int i = 0; i < 4; ++i) {
        const auto run_id = "run-break-a-" + std::to_string(i);
        const auto started_at = "2026020" + std::to_string(i + 1) + "T000000.000Z";
        const auto campaign_path = dir / (run_id + ".jsonl");
        write_file(campaign_path, fake_campaign_jsonl(run_id, started_at, 100));
        auto indexed = index_campaign(campaign_path, history_path, env_path);
        suite.check(indexed.ok && indexed.appended == 1, "ponto da série A deve indexar");
    }

    // Ambiente "test-env-2" resolve para host_class diferente -> series_key muda.
    write_file(env_path,
              R"({"schema":"modb.loadtest.environments","schema_version":1,"environments":)"
              R"([{"id":"test-env","label":"Test","kind":"local","host_class":"test-host-class"},)"
              R"({"id":"test-env-2","label":"Test2","kind":"local","host_class":"outro-host-class"}])"
              R"(})");
    auto campaign_b = fake_campaign_jsonl("run-break-b", "20260205T000000.000Z", 100);
    // Troca o ambiente do único caso para o registrado com host_class diferente.
    const auto pos = campaign_b.find("\"environment\":\"test-env\"");
    suite.check(pos != std::string::npos, "fixture deveria conter o campo environment esperado");
    campaign_b.replace(pos, std::string("\"environment\":\"test-env\"").size(),
                      "\"environment\":\"test-env-2\"");
    const auto campaign_b_path = dir / "run-break-b.jsonl";
    write_file(campaign_b_path, campaign_b);
    auto indexed_b = index_campaign(campaign_b_path, history_path, env_path);
    suite.check(indexed_b.ok && indexed_b.appended == 1, "ponto da série B deve indexar");

    auto trend =
        compute_trend(history_path, "load.create_only.embedded.10k", "ops_per_second", "create");
    suite.check(trend.ok, "compute_trend não deve falhar: " + trend.error);
    suite.check(trend.points.size() == 5, "devem existir 5 pontos (4 da série A + 1 da série B)");
    if (trend.points.size() == 5) {
        suite.check(!trend.points[3].series_break, "4o ponto ainda é da série A");
        suite.check(trend.points[4].series_break, "5o ponto deve marcar quebra de série");
        suite.check(trend.points[4].verdict == "insufficient",
                   "1o ponto de uma série nova nunca deve reutilizar a janela da série anterior");
    }
}

std::string read_file_content(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Encontra a última linha JSON do arquivo cujo `case_id` bate -- suficiente
// para os testes de rollup, que só indexam 1 caso por campanha.
const JsonValue* find_rollup_line(const std::vector<JsonValue>& parsed_lines) {
    for (const auto& line : parsed_lines) {
        if (line.get_string("case_id") == "load.create_only.embedded.10k") {
            return &line;
        }
    }
    return nullptr;
}

std::vector<JsonValue> parse_jsonl(const std::string& content) {
    std::vector<JsonValue> out;
    std::istringstream lines(content);
    std::string raw_line;
    while (std::getline(lines, raw_line)) {
        if (raw_line.empty()) {
            continue;
        }
        auto parsed = parse_json(raw_line);
        if (parsed.ok) {
            out.push_back(std::move(parsed.value));
        }
    }
    return out;
}

// Subfase F (§13.3 `windows`): rollup deve repassar o campo tal como o
// coletor gravou -- number a number, não um `null` hardcoded.
void test_rollup_reads_windows_field(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto history_path = dir / "series.jsonl";
    const auto env_path = dir / "environments.json";
    write_file(env_path, fake_environments_json());

    const std::string windows_json =
        R"({"first_ops_per_second":135000,"last_ops_per_second":128400,)"
        R"("slope_ops_per_second_per_min":-390,"first_p99_ns":30100,"last_p99_ns":34200})";
    const auto campaign_path = dir / "run-windows.jsonl";
    write_file(campaign_path,
              fake_campaign_jsonl("run-windows", "20260301T000000.000Z", 100,
                                 /*include_environment_record=*/true, windows_json));

    auto indexed = index_campaign(campaign_path, history_path, env_path);
    suite.check(indexed.ok && indexed.appended == 1, "campanha com windows deve indexar 1 ponto");

    const auto lines = parse_jsonl(read_file_content(history_path));
    const auto* rollup = find_rollup_line(lines);
    suite.check(rollup != nullptr, "rollup do caso deve existir no histórico");
    if (rollup) {
        const auto* windows = rollup->find("windows");
        suite.check(windows != nullptr && windows->is_object(),
                   "windows deve ser um objeto quando case_summary trouxe um");
        if (windows && windows->is_object()) {
            suite.check(windows->get_number("first_ops_per_second") == 135000,
                       "first_ops_per_second deve ser repassado sem alteração");
            suite.check(windows->get_number("slope_ops_per_second_per_min") == -390,
                       "slope negativo (degradação) deve ser preservado, não truncado");
        }
    }
}

// Sem `windows` no case_summary (caso curto, sem janela -- comportamento
// default de fake_campaign_jsonl), o rollup deve gravar `null`, nunca um
// objeto inventado com zeros.
void test_rollup_windows_null_when_absent(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto history_path = dir / "series.jsonl";
    const auto env_path = dir / "environments.json";
    write_file(env_path, fake_environments_json());

    const auto campaign_path = dir / "run-no-windows.jsonl";
    write_file(campaign_path, fake_campaign_jsonl("run-no-windows", "20260301T000000.000Z", 100));

    auto indexed = index_campaign(campaign_path, history_path, env_path);
    suite.check(indexed.ok && indexed.appended == 1, "campanha sem windows deve indexar 1 ponto");

    const auto lines = parse_jsonl(read_file_content(history_path));
    const auto* rollup = find_rollup_line(lines);
    suite.check(rollup != nullptr, "rollup do caso deve existir no histórico");
    if (rollup) {
        const auto* windows = rollup->find("windows");
        suite.check(windows != nullptr && windows->is_null(),
                   "windows deve ser null quando case_summary não trouxe janelas");
    }
}

std::string day_stamp(int day) {
    std::ostringstream oss;
    oss << "202601" << (day < 10 ? "0" : "") << day << "T000000.000Z";
    return oss.str();
}

// Subfase J (§13.7): critério de "pronto" #1 -- regressão sintética de 12%
// reprovada pelo gate por execução. 5 pontos-base em 100 (mediana=100),
// candidato em 88 -- queda de 12%, acima do fail_ratio de 10% de
// `ops_per_second`.
void test_gate_point_regression_fails(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto history_path = dir / "series.jsonl";
    const auto env_path = dir / "environments.json";
    write_file(env_path, fake_environments_json());

    for (int i = 0; i < 5; ++i) {
        const auto run_id = "run-gate-base-" + std::to_string(i);
        const auto campaign_path = dir / (run_id + ".jsonl");
        write_file(campaign_path, fake_campaign_jsonl(run_id, day_stamp(i + 1), 100));
        auto indexed = index_campaign(campaign_path, history_path, env_path);
        suite.check(indexed.ok && indexed.appended == 1, "ponto-base deve indexar");
    }
    const auto campaign_path = dir / "run-gate-regression.jsonl";
    write_file(campaign_path, fake_campaign_jsonl("run-gate-regression", day_stamp(6), 88));
    auto indexed = index_campaign(campaign_path, history_path, env_path);
    suite.check(indexed.ok && indexed.appended == 1, "candidato deve indexar");

    auto gate = compute_gate(history_path, "load.create_only.embedded.10k", "ops_per_second",
                             "create");
    suite.check(gate.ok, "compute_gate não deve falhar: " + gate.error);
    suite.check(gate.point_verdict == "fail",
               "queda de 12% (limiar 10% para ops_per_second) deve reprovar o gate pontual");
    suite.check(!gate.passed, "gate.passed deve ser false quando o gate pontual reprova");
    suite.check(gate.drift_verdict == "insufficient",
               "só 6 pontos não é histórico suficiente para deriva (precisa de 20+3)");
}

// Critério de "pronto" #2: deriva sintética de 15% em ~20 execuções
// detectada mesmo com todos os gates pontuais passando. 23 pontos caindo 1
// unidade por execução (100, 99, 98, ..., 78) -- cada passo individual fica
// bem abaixo do alert_ratio de 5% (mediana móvel de até 5 anteriores), mas a
// mediana das últimas 5 (78±) contra a mediana de ~20 execuções atrás (99±)
// já caiu quase 20%, acima do limiar de deriva de 15%.
void test_gate_drift_detects_slow_degradation(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto history_path = dir / "series.jsonl";
    const auto env_path = dir / "environments.json";
    write_file(env_path, fake_environments_json());

    constexpr int kPoints = 23;
    for (int i = 0; i < kPoints; ++i) {
        const auto run_id = "run-gate-drift-" + std::to_string(i);
        const auto campaign_path = dir / (run_id + ".jsonl");
        const double value = 100.0 - static_cast<double>(i);
        write_file(campaign_path, fake_campaign_jsonl(run_id, day_stamp(i + 1), value));
        auto indexed = index_campaign(campaign_path, history_path, env_path);
        suite.check(indexed.ok && indexed.appended == 1, "ponto de deriva deve indexar");
    }

    auto trend = compute_trend(history_path, "load.create_only.embedded.10k", "ops_per_second",
                               "create");
    suite.check(trend.ok, "compute_trend não deve falhar: " + trend.error);
    bool any_point_failed_or_alerted = false;
    for (const auto& p : trend.points) {
        if (p.verdict == "fail" || p.verdict == "alert") {
            any_point_failed_or_alerted = true;
        }
    }
    suite.check(!any_point_failed_or_alerted,
               "nenhum gate pontual isolado deveria acusar nada nesta série (queda suave demais "
               "por execução)");

    auto gate = compute_gate(history_path, "load.create_only.embedded.10k", "ops_per_second",
                             "create");
    suite.check(gate.ok, "compute_gate não deve falhar: " + gate.error);
    suite.check(gate.point_verdict != "fail",
               "o último ponto isolado não deveria reprovar (mesma lógica de compute_trend)");
    suite.check(gate.drift_verdict == "fail",
               "queda acumulada de ~20% em 20 execuções deve reprovar a deriva (limiar 15%)");
    suite.check(!gate.passed, "gate.passed deve ser false quando a deriva reprova");
}

void test_gate_rejects_unknown_metric(TestSuite& suite) {
    auto dir = make_temp_dir();
    auto gate = compute_gate(dir / "nao-existe.jsonl", "load.create_only.embedded.10k",
                             "metrica_fantasma", "create");
    suite.check(!gate.ok, "métrica desconhecida deve falhar");
    suite.check(!gate.error.empty(), "erro deve explicar a métrica desconhecida");
}

void test_gate_insufficient_history_passes(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto history_path = dir / "series.jsonl";
    const auto env_path = dir / "environments.json";
    write_file(env_path, fake_environments_json());

    const auto campaign_path = dir / "run-gate-single.jsonl";
    write_file(campaign_path, fake_campaign_jsonl("run-gate-single", day_stamp(1), 100));
    auto indexed = index_campaign(campaign_path, history_path, env_path);
    suite.check(indexed.ok && indexed.appended == 1, "ponto único deve indexar");

    auto gate = compute_gate(history_path, "load.create_only.embedded.10k", "ops_per_second",
                             "create");
    suite.check(gate.ok, "compute_gate não deve falhar: " + gate.error);
    suite.check(gate.passed,
               "histórico insuficiente nunca deve reprovar (§13.7: não bloquear CI por falta de "
               "dados)");
}

std::string fake_rollup_line(const std::string& series_key, const std::string& run_id,
                             const std::string& status, const std::string& started_at,
                             const std::string& raw_file) {
    std::ostringstream oss;
    oss << R"({"schema":"modb.loadtest.rollup","schema_version":1,"series_key":")" << series_key
        << R"(","case_id":"load.create_only.embedded.10k","run_id":")" << run_id
        << R"(","started_at":")" << started_at << R"(","status":")" << status
        << R"(","raw_file":")" << raw_file << R"("})";
    return oss.str();
}

void test_baseline_append_is_additive(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto path = dir / "baselines.json";

    suite.check(!std::filesystem::exists(path), "não deve existir antes da primeira marcação");
    auto loaded_missing = load_baselines(path);
    suite.check(loaded_missing.ok && loaded_missing.entries.empty(),
               "arquivo ausente deve carregar como lista vazia, não erro");

    BaselineEntry first{"seriesA", "run-1", "load.create_only.embedded.10k", "20260101T000000.000Z",
                        "primeira baseline conhecida"};
    suite.check(append_baseline(path, first), "primeira marcação deve funcionar");

    BaselineEntry second{"seriesA", "run-2", "load.create_only.embedded.10k", "20260102T000000.000Z",
                        "trocando a baseline por um run mais recente"};
    suite.check(append_baseline(path, second), "segunda marcação deve funcionar");

    auto loaded = load_baselines(path);
    suite.check(loaded.ok, "carregar depois de 2 marcações não deve falhar: " + loaded.error);
    suite.check(loaded.entries.size() == 2,
               "a marcação nova deve ACRESCENTAR, nunca substituir a anterior (§13.9)");
    suite.check(is_baseline_run(loaded.entries, "run-1") && is_baseline_run(loaded.entries, "run-2"),
               "os dois run_ids marcados devem ser reconhecidos como baseline");
    suite.check(!is_baseline_run(loaded.entries, "run-3"),
               "um run_id nunca marcado não deve ser reconhecido como baseline");
}

// Subfase J (§13.8): mantém os N mais recentes de cada série; mais antigos
// que isso são candidatos a remoção, EXCETO status=failed ou baseline
// marcada -- e nunca toca no rollup (`series.jsonl`) em si, só no bruto.
void test_prune_keeps_recent_failed_and_baseline(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto history_path = dir / "series.jsonl";
    const auto baselines_path = dir / "baselines.json";
    const auto raw_dir = dir / "raw";
    std::filesystem::create_directories(raw_dir);

    // 12 pontos: os 2 mais antigos (run-0/run-1) deveriam ser removíveis;
    // run-2 é status=failed (nunca remove); run-3 vira baseline (nunca
    // remove); run-2..run-11 (10 pontos) são a janela dos "10 mais
    // recentes" e ficam retidos só por estarem nela -- então só
    // run-0/run-1 sobram como candidatos reais de remoção.
    std::ostringstream history;
    for (int i = 0; i < 12; ++i) {
        const auto run_id = "run-" + std::to_string(i);
        const auto raw_file = "modb-load-" + run_id + ".jsonl";
        const auto status = (i == 2) ? "failed" : "completed";
        history << fake_rollup_line("seriesA", run_id, status, day_stamp(i + 1), raw_file) << '\n';
        write_file(raw_dir / raw_file, "conteudo bruto de teste\n");
    }
    write_file(history_path, history.str());
    suite.check(append_baseline(baselines_path, BaselineEntry{"seriesA", "run-3",
                                                             "load.create_only.embedded.10k",
                                                             "20260101T000000.000Z", "teste"}),
               "marcar run-3 como baseline não deve falhar");

    PruneOptions options;
    options.history_path = history_path;
    options.baselines_path = baselines_path;
    options.raw_dir = raw_dir;
    options.keep = 10;
    options.confirm = false;

    auto dry_run = prune_raw_files(options);
    suite.check(dry_run.ok, "prune (dry-run) não deve falhar: " + dry_run.error);
    std::size_t would_remove = 0;
    for (const auto& c : dry_run.candidates) {
        if (!c.kept) {
            ++would_remove;
        }
    }
    suite.check(would_remove == 2,
               "só run-00/run-01 (fora da janela de 10, não failed, não baseline) deveriam ser "
               "candidatos -- contou " +
                   std::to_string(would_remove));
    suite.check(dry_run.deleted.empty(), "dry-run nunca deve apagar nada de verdade");
    suite.check(std::filesystem::exists(raw_dir / "modb-load-run-0.jsonl"),
               "dry-run não deve ter apagado nenhum arquivo");

    options.confirm = true;
    auto confirmed = prune_raw_files(options);
    suite.check(confirmed.ok, "prune (confirm) não deve falhar: " + confirmed.error);
    suite.check(confirmed.deleted.size() == 2, "confirm deve remover exatamente os 2 candidatos");
    suite.check(!std::filesystem::exists(raw_dir / "modb-load-run-0.jsonl"),
               "run-0 deveria ter sido removido");
    suite.check(!std::filesystem::exists(raw_dir / "modb-load-run-1.jsonl"),
               "run-1 deveria ter sido removido");
    suite.check(std::filesystem::exists(raw_dir / "modb-load-run-2.jsonl"),
               "run-2 (failed) nunca deveria ser removido");
    suite.check(std::filesystem::exists(raw_dir / "modb-load-run-3.jsonl"),
               "run-3 (baseline) nunca deveria ser removido");
    suite.check(std::filesystem::exists(raw_dir / "modb-load-run-11.jsonl"),
               "run-11 (dentro da janela de 10 mais recentes) nunca deveria ser removido");

    // O rollup em si nunca é tocado (§13.8: "Rollups: nunca apagados").
    suite.check(std::filesystem::exists(history_path) &&
                   std::filesystem::file_size(history_path) == history.str().size(),
               "prune nunca deve modificar series.jsonl");
}

void test_trend_rejects_unknown_metric(TestSuite& suite) {
    suite.check(!find_metric("metrica_fantasma").has_value(),
               "métrica desconhecida deve devolver nullopt");
    suite.check(find_metric("ops_per_second").has_value(), "ops_per_second deve ser conhecida");
    suite.check(find_metric("write_amplification").has_value(),
               "write_amplification deve ser conhecida");
}

} // namespace

int main() {
    TestSuite suite;
    test_series_key_stable(suite);
    test_index_idempotent(suite);
    test_index_rejects_missing_provenance(suite);
    test_trend_median_and_verdict(suite);
    test_trend_series_break_resets_window(suite);
    test_rollup_reads_windows_field(suite);
    test_rollup_windows_null_when_absent(suite);
    test_gate_point_regression_fails(suite);
    test_gate_drift_detects_slow_degradation(suite);
    test_gate_rejects_unknown_metric(suite);
    test_gate_insufficient_history_passes(suite);
    test_baseline_append_is_additive(suite);
    test_prune_keeps_recent_failed_and_baseline(suite);
    test_trend_rejects_unknown_metric(suite);
    return suite.finish();
}
