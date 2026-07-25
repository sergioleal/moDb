#include "test_support.hpp"

#include "calibration.hpp"
#include "campaign.hpp"
#include "json_value.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace modb::loadtest;

namespace {

std::filesystem::path make_temp_dir() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
              ("modb-load-campaign-test-" + std::to_string(unique));
    std::filesystem::create_directories(dir);
    return dir;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::vector<std::string> nonempty_lines(const std::string& content) {
    std::vector<std::string> out;
    std::istringstream lines(content);
    std::string raw_line;
    while (std::getline(lines, raw_line)) {
        if (!raw_line.empty()) {
            out.push_back(raw_line);
        }
    }
    return out;
}

std::size_t count_records(const std::vector<std::string>& lines, const std::string& record,
                          const std::string& case_id) {
    std::size_t count = 0;
    for (const auto& line : lines) {
        auto parsed = parse_json(line);
        if (parsed.ok && parsed.value.get_string("record") == record &&
            parsed.value.get_string("case_id") == case_id) {
            ++count;
        }
    }
    return count;
}

CampaignOptions two_case_options(const std::filesystem::path& dir) {
    CampaignOptions options;
    options.selectors.case_ids = {"load.create_only.embedded.1k",
                                  "load.create_delete_forward.embedded.1k"};
    options.output_dir = dir;
    options.work_dir = dir;
    options.environments_file = dir / "no-such-environments.json";   // ausente = sem validação
    options.seed = 20260725;
    options.budget.accept_unknown_budget = true;
    options.argv_joined = "test";
    return options;
}

// Calibração fake com uma duração absurdamente alta para create_only@1k --
// suficiente para os testes de orçamento (Subfase H) sem depender de números
// reais medidos nem esperar uma execução longa de verdade.
std::filesystem::path write_fake_calibration(const std::filesystem::path& dir,
                                             std::uint64_t duration_ns) {
    std::ostringstream oss;
    oss << R"({"schema":"modb.loadtest.calibration","schema_version":1,)"
        << R"("platform":"test","arch":"x86_64","measured_at":"2026-07-25","entries":[)"
        << R"({"workload":"create_only","payload":"normal","scales":{)"
        << R"("1k":{"source":"measured","duration_ns":)" << duration_ns
        << R"(,"disk_peak_bytes":1000,"peak_rss_bytes":1000}}}]})";
    const auto path = dir / "fake-calibration.json";
    write_file(path, oss.str());
    return path;
}

CampaignOptions single_case_options(const std::filesystem::path& dir) {
    CampaignOptions options;
    options.selectors.case_ids = {"load.create_only.embedded.1k"};
    options.output_dir = dir;
    options.work_dir = dir;
    options.environments_file = dir / "no-such-environments.json";
    options.seed = 20260725;
    options.argv_joined = "test";
    return options;
}

// Subfase H (§10): um caso cuja estimativa CALIBRADA excede --max-duration
// deve ser pulado com `skipped_budget`, nunca executado -- e a campanha
// termina `partial`, nunca `completed`, mesmo com um único caso.
void test_budget_skips_case_exceeding_max_duration(TestSuite& suite) {
    auto dir = make_temp_dir();
    auto options = single_case_options(dir);
    // Calibração diz que este caso leva 1000s -- bem acima do limite de 1s.
    options.calibration_file = write_fake_calibration(dir, 1000ULL * 1'000'000'000ULL);
    options.budget.max_duration_seconds = 1;

    auto result = run_campaign(options);
    suite.check(result.ok, "campanha com 1 caso pulado ainda deve reportar ok (partial != failed): " +
                              result.error);
    suite.check(result.status == "partial",
               "caso pulado por orçamento deve deixar a campanha 'partial', não 'completed'");

    const auto content = read_file(result.result_path);
    const auto lines = nonempty_lines(content);
    suite.check(count_records(lines, "skipped_budget", "load.create_only.embedded.1k") == 1,
               "deve existir exatamente 1 skipped_budget para o caso calibrado acima do limite");
    suite.check(count_records(lines, "case_summary", "load.create_only.embedded.1k") == 0,
               "caso pulado nunca deve produzir case_summary (não foi executado)");
    suite.check(count_records(lines, "case_start", "load.create_only.embedded.1k") == 0,
               "caso pulado por orçamento não deve ter case_start (o skip acontece antes)");
}

// O mesmo caso, sem limite de orçamento configurado, deve rodar normalmente
// mesmo com a mesma calibração "cara" carregada -- os limites só mordem
// quando o operador de fato os configura (§10).
void test_budget_runs_case_when_no_limit_configured(TestSuite& suite) {
    auto dir = make_temp_dir();
    auto options = single_case_options(dir);
    options.calibration_file = write_fake_calibration(dir, 1000ULL * 1'000'000'000ULL);
    // Nenhum --max-* configurado.

    auto result = run_campaign(options);
    suite.check(result.ok, "sem limite configurado, o caso deve rodar normalmente: " + result.error);
    suite.check(result.status == "completed", "sem skip, a campanha deve completar normalmente");

    const auto lines = nonempty_lines(read_file(result.result_path));
    suite.check(count_records(lines, "case_summary", "load.create_only.embedded.1k") == 1,
               "sem limite, o caso deve ter rodado de verdade e produzido case_summary");
    suite.check(count_records(lines, "skipped_budget", "load.create_only.embedded.1k") == 0,
               "sem limite configurado, nunca deve haver skipped_budget");
}

void test_calibration_missing_file_is_ok_and_empty(TestSuite& suite) {
    auto dir = make_temp_dir();
    auto loaded = load_calibration(dir / "nao-existe.json");
    suite.check(loaded.ok, "arquivo de calibração ausente não deve ser erro");
    suite.check(loaded.table.entries.empty(), "tabela deve ficar vazia sem arquivo");
    suite.check(loaded.table.find("create_only", "normal", "100k") == nullptr,
               "find em tabela vazia deve devolver nullptr");
}

void test_calibration_load_and_find(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto path = write_fake_calibration(dir, 42'000'000'000ULL);
    auto loaded = load_calibration(path);
    suite.check(loaded.ok, "calibração válida deve carregar: " + loaded.error);

    const auto* point = loaded.table.find("create_only", "normal", "1k");
    suite.check(point != nullptr, "find deve achar a entrada gravada");
    if (point) {
        suite.check(point->duration_ns == 42'000'000'000ULL, "duration_ns deve bater com o gravado");
        suite.check(point->disk_peak_bytes == 1000, "disk_peak_bytes deve bater com o gravado");
    }
    suite.check(loaded.table.find("create_only", "normal", "100k") == nullptr,
               "escala não calibrada não deve ser encontrada");
    suite.check(loaded.table.find("crud_full", "normal", "1k") == nullptr,
               "workload não calibrado não deve ser encontrado");
}

// Verificação de ponta a ponta do critério de "pronto" da Subfase F
// (docs-process/PLANO_IMPLEMENTACAO_CARGA.md): interromper uma campanha no
// meio e retomar não deve reexecutar um caso já concluído. Constrói um
// `.partial` GENUÍNO (rodando `run_campaign` de verdade para 2 casos reais,
// pequenos o bastante para serem rápidos) e depois trunca o arquivo logo
// após o case_summary do 1o caso -- simulando o processo morto entre os dois
// casos -- para então chamar `resume_campaign` e conferir que só o 2o caso
// roda de novo.
void test_resume_does_not_rerun_completed_case(TestSuite& suite) {
    auto dir = make_temp_dir();

    auto first_run = run_campaign(two_case_options(dir));
    suite.check(first_run.ok, "campanha completa (sem interrupção) deve funcionar: " + first_run.error);
    suite.check(first_run.status == "completed", "campanha de 2 casos implementados deve completar");

    const auto full_content = read_file(first_run.result_path);
    auto lines = nonempty_lines(full_content);

    // Acha a linha do case_start do 2o caso (create_delete_forward) -- tudo
    // dali para frente (fases, case_summary, run_end) é descartado para
    // simular o processo morto NO MEIO do 2o caso (o cenário real de
    // "interromper 100k"): o 1o caso já concluiu, o 2o já tem case_start
    // gravado mas nenhum veredito ainda.
    std::size_t cut_at = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto parsed = parse_json(lines[i]);
        if (parsed.ok && parsed.value.get_string("record") == "case_start" &&
            parsed.value.get_string("case_id") == "load.create_delete_forward.embedded.1k") {
            cut_at = i + 1;
            break;
        }
    }
    suite.check(cut_at < lines.size(),
               "fixture deveria ter o case_start do 2o caso seguido de mais linhas (fases/summary/run_end)");

    std::ostringstream truncated;
    for (std::size_t i = 0; i < cut_at; ++i) {
        truncated << lines[i] << '\n';
    }
    const auto partial_path = dir / "resume-test.jsonl.partial";
    write_file(partial_path, truncated.str());

    // O arquivo final original ainda existe (viria de first_run) -- resume
    // promove PARA ESSE MESMO nome (derivado do próprio .partial), então
    // remove antes para não colidir com a checagem de "já existe".
    std::filesystem::remove(first_run.result_path);

    ResumeOptions resume_options;
    resume_options.partial_path = partial_path;
    resume_options.work_dir = dir;

    auto resumed = resume_campaign(resume_options);
    suite.check(resumed.ok, "resume deve completar: " + resumed.error);
    suite.check(resumed.status == "completed", "com os 2 casos implementados, status deve ser completed");
    suite.check(resumed.result_path == dir / "resume-test.jsonl",
               "resume deve promover para o nome final derivado do .partial");
    suite.check(std::filesystem::exists(resumed.result_path),
               "arquivo final deve existir após resume");
    suite.check(!std::filesystem::exists(partial_path),
               ".partial não deve sobreviver após promoção bem-sucedida");

    const auto resumed_content = read_file(resumed.result_path);
    const auto resumed_lines = nonempty_lines(resumed_content);

    suite.check(count_records(resumed_lines, "case_summary", "load.create_only.embedded.1k") == 1,
               "caso já concluído (create_only) deve ter exatamente 1 case_summary -- não reexecutado");
    suite.check(
        count_records(resumed_lines, "case_summary", "load.create_delete_forward.embedded.1k") == 1,
        "caso pendente (create_delete_forward) deve ter sido executado e ter exatamente 1 case_summary");
    // resume reemite case_start do caso pendente (o original já estava no
    // arquivo, truncado sem veredito) -- 2 linhas de case_start para o mesmo
    // case_id é esperado e inócuo: rollup.cpp trata case_start como upsert
    // (mesmos campos, sobrescreve identicamente), nunca duplica o caso no
    // rollup final.
    suite.check(count_records(resumed_lines, "case_start", "load.create_delete_forward.embedded.1k") == 2,
               "caso pendente deve ter o case_start original preservado + o reemitido por resume");

    std::size_t run_end_count = 0;
    for (const auto& line : resumed_lines) {
        auto parsed = parse_json(line);
        if (parsed.ok && parsed.value.get_string("record") == "run_end") {
            ++run_end_count;
        }
    }
    suite.check(run_end_count == 1, "arquivo final deve ter exatamente 1 run_end");
}

// Resume sobre um .partial onde os dois casos já concluíram deve recusar em
// vez de reexecutar tudo de novo silenciosamente.
void test_resume_rejects_when_nothing_pending(TestSuite& suite) {
    auto dir = make_temp_dir();

    auto first_run = run_campaign(two_case_options(dir));
    suite.check(first_run.ok, "campanha completa deve funcionar: " + first_run.error);

    const auto content = read_file(first_run.result_path);
    const auto partial_path = dir / "resume-complete.jsonl.partial";
    write_file(partial_path, content);   // arquivo INTEIRO, já com run_end -- nada pendente

    ResumeOptions resume_options;
    resume_options.partial_path = partial_path;
    resume_options.work_dir = dir;

    auto resumed = resume_campaign(resume_options);
    suite.check(!resumed.ok, "resume sem nenhum caso pendente deve falhar, não reexecutar tudo");
    suite.check(!resumed.error.empty(), "erro deve explicar que não há nada para retomar");
}

void test_resume_rejects_non_partial_extension(TestSuite& suite) {
    auto dir = make_temp_dir();
    const auto path = dir / "nao-e-partial.jsonl";
    write_file(path, "{}");

    ResumeOptions resume_options;
    resume_options.partial_path = path;

    auto resumed = resume_campaign(resume_options);
    suite.check(!resumed.ok, "resume deve recusar um caminho que não termina em .partial");
}

} // namespace

int main() {
    TestSuite suite;
    test_resume_does_not_rerun_completed_case(suite);
    test_resume_rejects_when_nothing_pending(suite);
    test_resume_rejects_non_partial_extension(suite);
    test_budget_skips_case_exceeding_max_duration(suite);
    test_budget_runs_case_when_no_limit_configured(suite);
    test_calibration_missing_file_is_ok_and_empty(suite);
    test_calibration_load_and_find(suite);
    return suite.finish();
}
