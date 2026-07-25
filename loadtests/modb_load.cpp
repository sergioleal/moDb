// CLI de testes de carga (docs/PLANO_TESTES_DE_CARGA.md). Subfases A/B/C/F:
// `run`, `list-cases`, `list-profiles`, `index`, `trend`, `resume` funcionam;
// `gate`/`compare` ainda não existem (Subfase J) e dizem isso claramente em
// vez de fingir que fizeram algo. `report` existe em forma mínima (CSV das
// linhas do rollup).
//
// Uso:
//   modb_load run --profile <nome> [seletores] [orçamento] [--output-dir DIR]
//                 [--work-dir DIR] [--seed N] [--dry-run] [--no-index]
//                 [--history-file PATH]
//   modb_load list-cases [seletores]
//   modb_load list-profiles
//   modb_load index <campanha.jsonl> [--history-file PATH] [--environments-file PATH]
//   modb_load trend --case ID --metric NOME [--phase NOME] [--history-file PATH]
//   modb_load report --case ID [--format csv|json] [--history-file PATH]
//   modb_load resume <arquivo.partial> [--work-dir DIR] [--seed N]

#include "campaign.hpp"
#include "history/index.hpp"
#include "history/trend.hpp"
#include "json_value.hpp"
#include "matrix.hpp"
#include "profiles.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using modb::loadtest::CampaignOptions;

void print_usage() {
    std::cerr
        << "Uso:\n"
        << "  modb_load run --profile <nome> [--scale a,b] [--workload a,b] [--target a,b]\n"
        << "                [--environment a,b] [--case id,id] [--filter SUBSTR]\n"
        << "                [--exclude SUBSTR] [--concurrency a,b] [--payload a,b]\n"
        << "                [--repeat N] [--seed N] [--output-dir DIR] [--work-dir DIR]\n"
        << "                [--environments-file PATH] [--max-duration N] [--max-disk-gb N]\n"
        << "                [--max-rss-mb N] [--accept-unknown-budget] [--dry-run]\n"
        << "                [--history-file PATH] [--no-index]\n"
        << "  modb_load list-cases [os mesmos seletores acima]\n"
        << "  modb_load list-profiles\n"
        << "  modb_load index <campanha.jsonl> [--history-file PATH] [--environments-file PATH]\n"
        << "  modb_load trend --case ID --metric NOME [--phase NOME] [--history-file PATH]\n"
        << "  modb_load report --case ID [--format csv|json] [--history-file PATH]\n"
        << "  modb_load resume <arquivo.partial> [--work-dir DIR] [--seed N]\n";
}

std::vector<std::string> split_comma(std::string_view value) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        auto comma = value.find(',', start);
        if (comma == std::string_view::npos) {
            out.emplace_back(value.substr(start));
            break;
        }
        out.emplace_back(value.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

void extend(std::vector<std::string>& target, std::string_view value) {
    auto parts = split_comma(value);
    target.insert(target.end(), parts.begin(), parts.end());
}

std::string join_argv(int argc, char** argv) {
    std::string out;
    for (int i = 0; i < argc; ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        out += argv[i];
    }
    return out;
}

// Preenche seletores comuns a `run` e `list-cases`. `history_file`/`no_index`
// só importam para `run` (indexação automática, §13.5) mas são aceitos aqui
// também para `list-cases` sem efeito, para manter um único parser.
bool parse_common_selectors(int argc, char** argv, int start, CampaignOptions& options,
                           std::filesystem::path& history_file, bool& no_index) {
    for (int i = start; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Falta valor para " << name << '\n';
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--profile") {
            options.profile = need("--profile");
        } else if (arg == "--scale") {
            extend(options.selectors.scale, need("--scale"));
        } else if (arg == "--workload") {
            extend(options.selectors.workload, need("--workload"));
        } else if (arg == "--target") {
            extend(options.selectors.target, need("--target"));
        } else if (arg == "--environment") {
            extend(options.selectors.environment, need("--environment"));
        } else if (arg == "--case") {
            extend(options.selectors.case_ids, need("--case"));
        } else if (arg == "--filter") {
            options.selectors.filter = need("--filter");
        } else if (arg == "--exclude") {
            extend(options.selectors.exclude, need("--exclude"));
        } else if (arg == "--concurrency") {
            extend(options.selectors.concurrency, need("--concurrency"));
        } else if (arg == "--payload") {
            extend(options.selectors.payload, need("--payload"));
        } else if (arg == "--repeat") {
            options.selectors.repeat = std::strtoull(need("--repeat"), nullptr, 10);
        } else if (arg == "--seed") {
            options.seed = std::strtoull(need("--seed"), nullptr, 10);
        } else if (arg == "--output-dir") {
            options.output_dir = need("--output-dir");
        } else if (arg == "--work-dir") {
            options.work_dir = need("--work-dir");
        } else if (arg == "--environments-file") {
            options.environments_file = need("--environments-file");
        } else if (arg == "--max-duration") {
            options.budget.max_duration_seconds = std::strtoull(need("--max-duration"), nullptr, 10);
        } else if (arg == "--max-disk-gb") {
            options.budget.max_disk_gb = std::strtoull(need("--max-disk-gb"), nullptr, 10);
        } else if (arg == "--max-rss-mb") {
            options.budget.max_rss_mb = std::strtoull(need("--max-rss-mb"), nullptr, 10);
        } else if (arg == "--accept-unknown-budget") {
            options.budget.accept_unknown_budget = true;
        } else if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "--history-file") {
            history_file = need("--history-file");
        } else if (arg == "--no-index") {
            no_index = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            std::exit(0);
        } else {
            std::cerr << "Argumento desconhecido: " << arg << '\n';
            print_usage();
            std::exit(2);
        }
    }
    return true;
}

int command_run(int argc, char** argv) {
    CampaignOptions options;
    options.argv_joined = join_argv(argc, argv);
    std::filesystem::path history_file{"load-history/series.jsonl"};
    bool no_index = false;
    parse_common_selectors(argc, argv, 2, options, history_file, no_index);

    if (!options.profile && options.selectors.case_ids.empty()) {
        std::cerr << "Erro: informe --profile ou --case\n";
        return 2;
    }

    auto result = modb::loadtest::run_campaign(options);
    if (options.dry_run) {
        if (!result.ok) {
            std::cerr << "Erro: " << result.error << '\n';
            return 2;
        }
        std::cerr << result.rendered_plan;
        return 0;
    }
    if (!result.ok && result.result_path.empty()) {
        // Falhou antes de escrever qualquer arquivo (composição de seletores,
        // ambiente desconhecido, ou gate de orçamento).
        std::cerr << "Erro: " << result.error << '\n';
        return 2;
    }
    if (!result.error.empty()) {
        std::cerr << "Erro: " << result.error << '\n';
    }
    std::cout << "Resultado: " << result.result_path.string() << "  run_id=" << result.run_id
              << "  status=" << result.status << '\n';

    // Indexação automática ao final de `run` (§13.5); `--no-index` desliga.
    if (!no_index) {
        auto indexed = modb::loadtest::index_campaign(result.result_path, history_file,
                                                       options.environments_file);
        if (!indexed.ok) {
            std::cerr << "Aviso: indexação falhou: " << indexed.error << '\n';
        } else {
            std::cerr << "Índice: " << indexed.appended << " ponto(s) novo(s), "
                      << indexed.skipped_duplicate << " duplicata(s), " << indexed.rejected
                      << " rejeitado(s) em " << history_file.string() << '\n';
            for (const auto& reason : indexed.rejection_reasons) {
                std::cerr << "  rejeitado: " << reason << '\n';
            }
        }
    }

    return result.status == "failed" ? 1 : 0;
}

int command_list_cases(int argc, char** argv) {
    CampaignOptions options;
    options.argv_joined = join_argv(argc, argv);
    std::filesystem::path history_file{"load-history/series.jsonl"};
    bool no_index = false;
    parse_common_selectors(argc, argv, 2, options, history_file, no_index);

    if (!options.profile && options.selectors.case_ids.empty()) {
        std::cerr << "Erro: informe --profile ou --case\n";
        return 2;
    }

    auto resolved = modb::loadtest::resolve_cases(options);
    if (!resolved.ok) {
        std::cerr << "Erro: " << resolved.error << '\n';
        return 2;
    }
    std::cerr << modb::loadtest::render_case_plan(resolved.cases);
    return 0;
}

int command_list_profiles() {
    for (const auto& name : modb::loadtest::list_profile_names()) {
        std::cout << name << '\n';
    }
    return 0;
}

int command_index(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Uso: modb_load index <campanha.jsonl> [--history-file PATH] "
                    "[--environments-file PATH]\n";
        return 2;
    }
    const std::filesystem::path campaign_path{argv[2]};
    std::filesystem::path history_file{"load-history/series.jsonl"};
    std::filesystem::path environments_file{"loadtests/environments.json"};
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Falta valor para " << name << '\n';
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--history-file") {
            history_file = need("--history-file");
        } else if (arg == "--environments-file") {
            environments_file = need("--environments-file");
        } else {
            std::cerr << "Argumento desconhecido: " << arg << '\n';
            return 2;
        }
    }

    auto indexed = modb::loadtest::index_campaign(campaign_path, history_file, environments_file);
    if (!indexed.ok) {
        std::cerr << "Erro: " << indexed.error << '\n';
        return 1;
    }
    std::cout << indexed.appended << " ponto(s) novo(s), " << indexed.skipped_duplicate
              << " duplicata(s), " << indexed.rejected << " rejeitado(s) em "
              << history_file.string() << '\n';
    for (const auto& reason : indexed.rejection_reasons) {
        std::cerr << "  rejeitado: " << reason << '\n';
    }
    return indexed.rejected > 0 ? 1 : 0;
}

int command_trend(int argc, char** argv) {
    std::string case_id, metric_id, phase;
    std::filesystem::path history_file{"load-history/series.jsonl"};
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Falta valor para " << name << '\n';
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--case") {
            case_id = need("--case");
        } else if (arg == "--metric") {
            metric_id = need("--metric");
        } else if (arg == "--phase") {
            phase = need("--phase");
        } else if (arg == "--history-file") {
            history_file = need("--history-file");
        } else {
            std::cerr << "Argumento desconhecido: " << arg << '\n';
            return 2;
        }
    }
    if (case_id.empty() || metric_id.empty()) {
        std::cerr << "Uso: modb_load trend --case ID --metric NOME [--phase NOME] "
                    "[--history-file PATH]\n";
        return 2;
    }

    auto metric = modb::loadtest::find_metric(metric_id);
    if (!metric) {
        std::cerr << "Erro: métrica desconhecida: " << metric_id << ". Conhecidas: ";
        for (const auto& id : modb::loadtest::known_metric_ids()) {
            std::cerr << id << ' ';
        }
        std::cerr << '\n';
        return 2;
    }

    auto trend = modb::loadtest::compute_trend(history_file, case_id, metric_id, phase);
    if (!trend.ok) {
        std::cerr << "Erro: " << trend.error << '\n';
        return 1;
    }
    if (trend.points.empty()) {
        std::cout << "Nenhum ponto para o caso '" << case_id << "' em " << history_file.string()
                  << '\n';
        return 0;
    }
    std::cout << modb::loadtest::render_trend(trend, *metric);
    return 0;
}

int command_report(int argc, char** argv) {
    std::string case_id, format = "csv";
    std::filesystem::path history_file{"load-history/series.jsonl"};
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Falta valor para " << name << '\n';
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--case") {
            case_id = need("--case");
        } else if (arg == "--format") {
            format = need("--format");
        } else if (arg == "--history-file") {
            history_file = need("--history-file");
        } else {
            std::cerr << "Argumento desconhecido: " << arg << '\n';
            return 2;
        }
    }
    if (case_id.empty()) {
        std::cerr << "Uso: modb_load report --case ID [--format csv|json] [--history-file PATH]\n";
        return 2;
    }
    if (format != "csv" && format != "json") {
        std::cerr << "Erro: --format deve ser csv ou json\n";
        return 2;
    }
    if (!std::filesystem::exists(history_file)) {
        std::cerr << "Erro: arquivo histórico não encontrado: " << history_file.string() << '\n';
        return 1;
    }

    // Forma mínima (§13.6): exporta linha a linha os campos de escopo de
    // caso (totals não depende de --phase) para análise externa --
    // planilha/notebook fazem o resto. Métricas por fase completas seguem
    // reservadas ao dashboard e a `trend`.
    std::ifstream file(history_file, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::istringstream lines(buffer.str());
    std::string raw_line;
    std::vector<std::string> rows;
    while (std::getline(lines, raw_line)) {
        if (raw_line.empty()) {
            continue;
        }
        auto parsed = modb::loadtest::parse_json(raw_line);
        if (!parsed.ok || !parsed.value.is_object()) {
            std::cerr << "Erro: linha inválida em " << history_file.string() << ": " << parsed.error
                      << '\n';
            return 1;
        }
        if (parsed.value.get_string("case_id") != case_id) {
            continue;
        }
        const auto* totals = parsed.value.find("totals");
        const auto started_at = parsed.value.get_string("started_at");
        const auto commit_short = parsed.value.get_string("commit_short");
        const auto series_key = parsed.value.get_string("series_key");
        const auto status = parsed.value.get_string("status");
        const auto duration_ns = totals ? totals->get_number("duration_ns") : 0.0;
        const auto peak_disk_bytes = totals ? totals->get_number("peak_disk_bytes") : 0.0;
        const auto write_amplification = totals ? totals->get_number("write_amplification") : 0.0;

        if (format == "csv") {
            std::ostringstream row;
            row << started_at << ';' << commit_short << ';' << series_key << ';' << status << ';'
                << duration_ns << ';' << peak_disk_bytes << ';' << write_amplification;
            rows.push_back(row.str());
        } else {
            std::ostringstream row;
            row << "{\"started_at\":\"" << started_at << "\",\"commit_short\":\"" << commit_short
                << "\",\"series_key\":\"" << series_key << "\",\"status\":\"" << status
                << "\",\"total_duration_ns\":" << duration_ns
                << ",\"peak_disk_bytes\":" << peak_disk_bytes
                << ",\"write_amplification\":" << write_amplification << "}";
            rows.push_back(row.str());
        }
    }

    if (rows.empty()) {
        std::cerr << "Nenhum ponto para o caso '" << case_id << "' em " << history_file.string()
                  << '\n';
        return 0;
    }
    if (format == "csv") {
        std::cout << "started_at;commit_short;series_key;status;total_duration_ns;"
                    "peak_disk_bytes;write_amplification\n";
    }
    for (const auto& row : rows) {
        std::cout << row << '\n';
    }
    return 0;
}

int command_resume(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Uso: modb_load resume <arquivo.partial> [--work-dir DIR] [--seed N]\n";
        return 2;
    }
    modb::loadtest::ResumeOptions options;
    options.partial_path = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Falta valor para " << name << '\n';
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--work-dir") {
            options.work_dir = need("--work-dir");
        } else if (arg == "--seed") {
            options.seed_override = std::strtoull(need("--seed"), nullptr, 10);
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Argumento desconhecido: " << arg << '\n';
            return 2;
        }
    }

    auto result = modb::loadtest::resume_campaign(options);
    if (!result.ok && result.result_path.empty()) {
        std::cerr << "Erro: " << result.error << '\n';
        return 2;
    }
    if (!result.error.empty()) {
        std::cerr << "Erro: " << result.error << '\n';
    }
    std::cout << "Resultado: " << result.result_path.string() << "  run_id=" << result.run_id
              << "  status=" << result.status << '\n';
    return result.status == "failed" ? 1 : 0;
}

int command_not_implemented(std::string_view command, std::string_view subfase) {
    std::cerr << "modb_load " << command << ": ainda não implementado (ver "
             << "docs/PLANO_TESTES_DE_CARGA.md, Subfase " << subfase << ").\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::string_view command{argv[1]};
    if (command == "run") {
        return command_run(argc, argv);
    }
    if (command == "list-cases") {
        return command_list_cases(argc, argv);
    }
    if (command == "list-profiles") {
        return command_list_profiles();
    }
    if (command == "index") {
        return command_index(argc, argv);
    }
    if (command == "trend") {
        return command_trend(argc, argv);
    }
    if (command == "report") {
        return command_report(argc, argv);
    }
    if (command == "resume") {
        return command_resume(argc, argv);
    }
    if (command == "gate") {
        return command_not_implemented(command, "J");
    }
    if (command == "compare") {
        return command_not_implemented("compare", "J");
    }
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage();
        return 0;
    }
    print_usage();
    return 2;
}
