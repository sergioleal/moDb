#include "test_support.hpp"

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
    return suite.finish();
}
