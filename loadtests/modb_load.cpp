// CLI de testes de carga (docs/PLANO_TESTES_DE_CARGA.md). Subfase A/B:
// `run`, `list-cases`, `list-profiles` funcionam; `resume`/`index`/`trend`/
// `report`/`gate`/`compare` ainda não existem (Subfases C/F/J) e dizem isso
// claramente em vez de fingir que fizeram algo.
//
// Uso:
//   modb_load run --profile <nome> [seletores] [orçamento] [--output-dir DIR]
//                 [--work-dir DIR] [--seed N] [--dry-run]
//   modb_load list-cases [seletores]
//   modb_load list-profiles

#include "campaign.hpp"
#include "matrix.hpp"
#include "profiles.hpp"

#include <cstdlib>
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
        << "  modb_load list-cases [os mesmos seletores acima]\n"
        << "  modb_load list-profiles\n";
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

// Preenche seletores comuns a `run` e `list-cases`. Devolve false (e já
// imprime o motivo) se um argumento exigir valor e não houver.
bool parse_common_selectors(int argc, char** argv, int start, CampaignOptions& options) {
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
    parse_common_selectors(argc, argv, 2, options);

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
    return result.status == "failed" ? 1 : 0;
}

int command_list_cases(int argc, char** argv) {
    CampaignOptions options;
    options.argv_joined = join_argv(argc, argv);
    parse_common_selectors(argc, argv, 2, options);

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
    if (command == "resume") {
        return command_not_implemented("resume", "F");
    }
    if (command == "index" || command == "trend" || command == "report" || command == "gate") {
        return command_not_implemented(command, "C/J");
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
