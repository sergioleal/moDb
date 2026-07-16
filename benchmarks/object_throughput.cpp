// Benchmark de carga do ObjectStore: cria N mil objetos, apaga todos e reporta
// o throughput (objetos por segundo) de cada fase. Mede o motor de verdade
// (create/remove passando por codec + heap + mapa de identidade + DBRT), não é
// um teste funcional — fica fora do ctest.
//
// Uso:
//   modb_object_bench [milhares] [arquivo]
//     milhares : quantos MIL objetos criar/apagar (padrao 10 => 10.000)
//     arquivo  : caminho do banco (padrao: temp, removido ao final)

#include "modb/object/object_store.hpp"
#include "modb/storage/page_file.hpp"

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
// std::error_code na limpeza.
#include <system_error>
// Nome e montagem de rótulos.
#include <string>
#include <string_view>
// Guarda os ids criados para depois apagá-los.
#include <vector>

using namespace modb;
using namespace modb::object;
using modb::storage::PageFile;

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

// Executa o benchmark completo. Recebe o arquivo por referência já criado, para
// que PageFile/ObjectStore sejam destruídos por quem chama antes da limpeza.
int run_benchmark(std::uint64_t total, const std::filesystem::path& path) {
    auto file = PageFile::create(path);
    if (!file) {
        return fail("PageFile::create", file.error());
    }
    auto store = ObjectStore::create(*file);
    if (!store) {
        return fail("ObjectStore::create", store.error());
    }
    auto type = TypeDefinition::create(
        "Item", std::vector<AttributeDefinition>{
                    AttributeDefinition{.id = FieldId{1}, .name = "seq",
                                      .type = AttributeType::int64, .nullable = false},
                    AttributeDefinition{.id = FieldId{2}, .name = "label",
                                      .type = AttributeType::string, .nullable = false},
                });
    if (!type) {
        return fail("TypeDefinition::create", type.error());
    }
    auto type_id = store->register_type(std::move(*type));
    if (!type_id) {
        return fail("register_type", type_id.error());
    }
    auto type_ref = store->find_type(*type_id);
    if (!type_ref) {
        return fail("find_type", type_ref.error());
    }
    const auto& item_type = type_ref->get();

    std::vector<ObjectId> ids;
    ids.reserve(total);

    std::cout << "moDb — benchmark de throughput do ObjectStore\n";
    std::cout << "Objetos:   " << total << "\n";
    std::cout << "Arquivo:   " << path.string() << "\n\n";

    // --- fase CREATE ---
    const auto create_start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < total; ++i) {
        FieldValues fields{
            {FieldId{1}, AttributeValue{static_cast<std::int64_t>(i)}},
            {FieldId{2}, AttributeValue{"item-" + std::to_string(i)}},
        };
        auto id = store->create_object(item_type, std::move(fields));
        if (!id) {
            return fail("create_object", id.error());
        }
        ids.push_back(*id);
    }
    const auto create_end = std::chrono::steady_clock::now();
    if (auto flushed = file->flush(); !flushed) {
        return fail("flush (create)", flushed.error());
    }
    const auto create_flush_end = std::chrono::steady_clock::now();

    // Tamanho do arquivo no pico (após criar tudo).
    std::error_code size_error;
    const auto peak_bytes = std::filesystem::file_size(path, size_error);

    // --- fase DELETE ---
    const auto delete_start = std::chrono::steady_clock::now();
    for (const auto id : ids) {
        if (auto removed = store->remove(id); !removed) {
            return fail("remove", removed.error());
        }
    }
    const auto delete_end = std::chrono::steady_clock::now();
    if (auto flushed = file->flush(); !flushed) {
        return fail("flush (delete)", flushed.error());
    }
    const auto delete_flush_end = std::chrono::steady_clock::now();

    // A verificação fica fora da medição: o benchmark só termina com sucesso
    // quando todos os objetos criados foram efetivamente apagados.
    std::uint64_t remaining = 0;
    auto scanned = store->scan([&remaining](const DecodedObject&) -> Result<void> {
        ++remaining;
        return {};
    });
    if (!scanned) {
        return fail("scan de verificacao", scanned.error());
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
    // --- argumentos ---
    if (argc > 3) {
        std::cerr << "Uso: modb_object_bench [milhares] [arquivo]\n";
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

    // Caminho: o segundo argumento, ou um arquivo temporário limpo ao final.
    const bool cleanup = argc < 3;
    std::filesystem::path path;
    if (argc >= 3) {
        path = argv[2];
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
    const int status = run_benchmark(total, path);

    if (cleanup) {
        std::filesystem::remove(path, ignored);
    }
    return status;
}
