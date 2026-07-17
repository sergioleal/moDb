// Benchmark de carga do ObjectStore: cria N mil objetos, apaga todos e reporta
// o throughput (objetos por segundo) de cada fase. Mede o motor de verdade
// (create/remove passando por codec + heap + mapa de identidade + DBRT), não é
// um teste funcional — fica fora do ctest.
//
// Uso:
//   modb_object_bench [milhares] [arquivo]
//     milhares : quantos MIL objetos criar/apagar (padrao 10 => 10.000)
//     arquivo  : caminho do banco (padrao: temp, removido ao final)

#include "modb/object/database.hpp"

// Relógio monotônico para medir tempo de parede.
#include <chrono>
// std::from_chars para ler o argumento numérico.
#include <charconv>
// Inteiros de largura fixa.
#include <cstdint>
// Caminhos e tamanho do arquivo.
#include <filesystem>
// Formatação da saída.
#include <iomanip>
#include <iostream>
// Limites usados para impedir overflow no argumento em milhares.
#include <limits>
#include <memory>
// std::error_code na limpeza.
#include <system_error>
// Nome e montagem de rótulos.
#include <string>
#include <string_view>
// Guarda os ids criados para depois apagá-los.
#include <vector>

using namespace modb;
using namespace modb::object;

namespace {

// Segundos (double) entre dois instantes do relógio monotônico.
double seconds_between(std::chrono::steady_clock::time_point a,
                       std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

// Imprime uma linha de fase: rótulo, contagem, tempo e throughput.
void report_phase(std::string_view label, std::uint64_t count, double secs) {
    const double per_second = secs > 0.0 ? static_cast<double>(count) / secs : 0.0;
    std::cout << std::left << std::setw(10) << label << std::right << std::setw(10) << count
              << " objetos em " << std::fixed << std::setprecision(3) << std::setw(9) << secs
              << " s  =  " << std::setprecision(0) << std::setw(11) << per_second
              << " objetos/s\n";
}

// Reporta um erro do motor e devolve o código de saída.
int fail(std::string_view what, const Error& error) {
    std::cerr << "Erro em " << what << ": " << error.message << '\n';
    return 1;
}

struct Item {
    std::int64_t seq{};
    std::string label;
};

BindingBuilder<Item> item_binding() {
    BindingBuilder<Item> builder{"Item"};
    builder.field<1>("seq", &Item::seq).field<2>("label", &Item::label);
    return builder;
}

// Executa o benchmark completo. `stride` escolhe a ordem de remoção: <=1 apaga
// em sequência (esvazia páginas rápido); >1 apaga em passada — a cada `stride`
// posições, depois volta ao offset seguinte —, espalhando as remoções por
// muitas páginas e mantendo-as vivas e fragmentadas por mais tempo.
int run_benchmark(std::uint64_t total, std::uint64_t stride, const std::filesystem::path& path) {
    auto created = Database::create(path);
    if (!created) {
        return fail("Database::create", created.error());
    }
    auto database = std::make_shared<Database>(std::move(*created));
    auto database_id = DatabaseRegistry::instance().attach(database);
    if (!database_id) {
        return fail("DatabaseRegistry::attach", database_id.error());
    }
    if (auto bound = database->bind(item_binding()); !bound) {
        return fail("bind", bound.error());
    }

    std::vector<ObjectId> ids;
    ids.reserve(total);

    std::cout << "moDb — benchmark de throughput do ObjectStore\n";
    std::cout << "Objetos:   " << total << "\n";
    std::cout << "Delete:    " << (stride <= 1 ? "sequencial" : "em passada (stride " +
                                                                    std::to_string(stride) + ")")
              << "\n";
#ifdef MODB_EAGER_COMPACT
    std::cout << "Compact:   ANSIOSA (toggle de benchmark)\n";
#else
    std::cout << "Compact:   preguicosa\n";
#endif
    std::cout << "Arquivo:   " << path.string() << "\n\n";

    // --- fase CREATE ---
    auto create_tx = database->begin();
    if (!create_tx) {
        return fail("begin (create)", create_tx.error());
    }
    const auto create_start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < total; ++i) {
        auto id = database->create(*create_tx, Item{static_cast<std::int64_t>(i), "item-" + std::to_string(i)});
        if (!id) {
            return fail("create", id.error());
        }
        ids.push_back(id->id());
    }
    const auto create_end = std::chrono::steady_clock::now();
    if (auto committed = create_tx->commit(); !committed) {
        return fail("commit (create)", committed.error());
    }
    const auto create_flush_end = std::chrono::steady_clock::now();

    // Tamanho do arquivo no pico (após criar tudo).
    std::error_code size_error;
    const auto peak_bytes = std::filesystem::file_size(path, size_error);

    // --- fase DELETE ---
    // Remove um id e propaga um eventual erro, para reuso nas duas ordens.
    auto delete_tx = database->begin();
    if (!delete_tx) {
        return fail("begin (delete)", delete_tx.error());
    }
    const auto remove_at = [&database, &delete_tx](ObjectId id) -> Result<void> {
        if (auto removed = database->remove(*delete_tx, id); !removed) {
            return std::unexpected(removed.error());
        }
        return {};
    };
    const auto delete_start = std::chrono::steady_clock::now();
    if (stride <= 1) {
        // Sequencial: apaga na ordem de criação.
        for (const auto id : ids) {
            if (auto removed = remove_at(id); !removed) {
                return fail("remove", removed.error());
            }
        }
    } else {
        // Em passada: offset 0 pega 0, stride, 2*stride...; depois offset 1, etc.
        for (std::uint64_t offset = 0; offset < stride; ++offset) {
            for (std::uint64_t i = offset; i < ids.size(); i += stride) {
                if (auto removed = remove_at(ids[i]); !removed) {
                    return fail("remove", removed.error());
                }
            }
        }
    }
    const auto delete_end = std::chrono::steady_clock::now();
    if (auto committed = delete_tx->commit(); !committed) {
        return fail("commit (delete)", committed.error());
    }
    const auto delete_flush_end = std::chrono::steady_clock::now();

    // A verificação fica fora da medição: o benchmark só termina com sucesso
    // quando todos os objetos criados foram efetivamente apagados.
    std::uint64_t remaining = 0;
    for (const auto id : ids) {
        if (database->get<Item>(id)) {
            ++remaining;
        }
    }
    if (remaining != 0) {
        std::cerr << "Erro: " << remaining << " objeto(s) permaneceram apos o delete.\n";
        return 1;
    }

    // --- relatório ---
    const double create_secs = seconds_between(create_start, create_end);
    const double delete_secs = seconds_between(delete_start, delete_end);
    report_phase("Create", total, create_secs);
    std::cout << "  flush:  " << std::fixed << std::setprecision(1)
              << seconds_between(create_end, create_flush_end) * 1000.0 << " ms\n";
    report_phase("Delete", total, delete_secs);
    std::cout << "  flush:  " << std::fixed << std::setprecision(1)
              << seconds_between(delete_end, delete_flush_end) * 1000.0 << " ms\n";
    report_phase("Combinado", total * 2, create_secs + delete_secs);

    const double total_secs = seconds_between(create_start, delete_flush_end);
    std::cout << "\nTotal:     " << std::fixed << std::setprecision(3) << total_secs << " s\n";
    std::cout << "Pico do arquivo: " << std::setprecision(2)
              << static_cast<double>(peak_bytes) / (1024.0 * 1024.0) << " MiB ("
              << peak_bytes / (total > 0 ? total : 1) << " bytes/objeto)\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    // --- argumentos: [milhares] [stride] [arquivo] ---
    if (argc > 4) {
        std::cerr << "Uso: modb_object_bench [milhares] [stride] [arquivo]\n";
        return 2;
    }

    std::uint64_t thousands = 10;
    if (argc >= 2) {
        const std::string_view text{argv[1]};
        auto result = std::from_chars(text.data(), text.data() + text.size(), thousands);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
            thousands == 0 || thousands > std::numeric_limits<std::uint64_t>::max() / 2000) {
            std::cerr << "Argumento invalido: informe uma quantidade de milhares valida e "
                         "maior que zero.\n";
            return 2;
        }
    }
    const std::uint64_t total = thousands * 1000;

    // stride de remoção: 0/1 = sequencial; >1 = em passada.
    std::uint64_t stride = 0;
    if (argc >= 3) {
        const std::string_view text{argv[2]};
        auto result = std::from_chars(text.data(), text.data() + text.size(), stride);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            std::cerr << "Argumento invalido: stride deve ser um numero.\n";
            return 2;
        }
    }

    // Caminho: o terceiro argumento, ou um arquivo temporário limpo ao final.
    const bool cleanup = argc < 4;
    std::filesystem::path path;
    if (argc >= 4) {
        path = argv[3];
        if (std::filesystem::exists(path)) {
            std::cerr << "O arquivo informado ja existe; escolha outro caminho: "
                      << path.string() << '\n';
            return 2;
        }
    } else {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("modb-bench-" + std::to_string(unique) + ".modb");
    }
    std::error_code ignored;

    // run_benchmark destrói PageFile/ObjectStore ao retornar, liberando o
    // arquivo antes da limpeza (necessário no Windows).
    const int status = run_benchmark(total, stride, path);

    if (cleanup) {
        std::filesystem::remove(path, ignored);
    }
    return status;
}
