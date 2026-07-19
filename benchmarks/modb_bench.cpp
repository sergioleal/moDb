// Coordenador de campanhas de benchmark (Fase 10A).
//
// Uso:
//   modb_bench run --profile smoke [--seed N] [--output-dir DIR] [--filter SUBSTR]
//   modb_bench compare <baseline.jsonl> <candidate.jsonl>
//   modb_bench list-profiles

#include "runner/campaign.hpp"
#include "runner/profile.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage() {
    std::cerr
        << "Uso:\n"
        << "  modb_bench run --profile <smoke|standard|diagnostic> [--seed N]\n"
        << "                 [--output-dir DIR] [--work-dir DIR] [--filter SUBSTR]\n"
        << "  modb_bench compare <baseline.jsonl> <candidate.jsonl>\n"
        << "  modb_bench list-profiles\n";
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

int command_run(int argc, char** argv) {
    modb::bench::CampaignOptions options;
    options.argv_joined = join_argv(argc, argv);
    for (int i = 2; i < argc; ++i) {
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
        } else if (arg == "--seed") {
            options.seed = std::strtoull(need("--seed"), nullptr, 10);
        } else if (arg == "--output-dir") {
            options.output_dir = need("--output-dir");
        } else if (arg == "--work-dir") {
            options.work_dir = need("--work-dir");
        } else if (arg == "--filter") {
            options.filter = need("--filter");
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Argumento desconhecido: " << arg << '\n';
            print_usage();
            return 2;
        }
    }

    const auto result = modb::bench::run_campaign(options);
    if (!result.ok) {
        if (!result.error.empty()) {
            std::cerr << "Erro: " << result.error << '\n';
        }
        return 1;
    }
    return 0;
}

int command_compare(int argc, char** argv) {
    if (argc != 4) {
        print_usage();
        return 2;
    }
    const auto compared =
        modb::bench::compare_campaigns(argv[2], argv[3]);
    if (!compared.ok) {
        std::cerr << "Erro: " << compared.error << '\n';
        return 1;
    }
    int worst = 0;
    for (const auto& delta : compared.deltas) {
        std::cout << delta.parameters_key << "  baseline=" << delta.baseline_ops_per_sec
                  << "  candidate=" << delta.candidate_ops_per_sec
                  << "  delta=" << delta.delta_percent << "%  " << delta.verdict << '\n';
        if (delta.verdict == "fail") {
            worst = std::max(worst, 2);
        } else if (delta.verdict == "alert" || delta.verdict == "incompatible") {
            worst = std::max(worst, 1);
        }
    }
    if (compared.deltas.empty()) {
        std::cout << "Nenhum scenario_summary comparavel encontrado.\n";
        return 1;
    }
    return worst == 2 ? 1 : 0;
}

int command_list_profiles() {
    for (const auto& name : modb::bench::list_profile_names()) {
        std::cout << name << '\n';
    }
    return 0;
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
    if (command == "compare") {
        return command_compare(argc, argv);
    }
    if (command == "list-profiles") {
        return command_list_profiles();
    }
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage();
        return 0;
    }
    print_usage();
    return 2;
}
