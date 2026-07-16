// Importa Row e Value usados nos registros dos testes.
#include "modb/row.hpp"
#include "modb/value.hpp"
// Importa os codecs usados antes e depois da página.
#include "modb/storage/codec.hpp"
// Importa o arquivo usado no teste de persistência.
#include "modb/storage/page_file.hpp"
// Importa a slotted page exercitada neste arquivo.
#include "modb/storage/slotted_page.hpp"

// Importa as funções simples de verificação.
#include "test_support.hpp"

// Disponibiliza um valor variável para nomes temporários.
#include <chrono>
// Disponibiliza std::byte e std::size_t.
#include <cstddef>
// Disponibiliza caminhos e remoção de arquivos.
#include <filesystem>
// Disponibiliza a impressão do caminho temporário.
#include <iostream>
// Disponibiliza o limite usado para testar o esgotamento da geração.
#include <limits>
// Disponibiliza nomes de arquivos e textos de registros.
#include <string>
// Disponibiliza std::error_code para limpeza sem exceções.
#include <system_error>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza registros binários artificiais.
#include <vector>

namespace {

// Remove automaticamente o banco criado pelo teste de persistência.
class TemporaryDatabase {
public:
    // Gera um caminho diferente para cada execução.
    TemporaryDatabase() {
        // Obtém um número variável de um relógio monotônico.
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        // Combina a pasta temporária com o nome exclusivo.
        path_ = std::filesystem::temp_directory_path() /
                ("modb-slotted-page-" + std::to_string(unique) + ".modb");
    }

    // Remove o arquivo mesmo quando uma verificação falha.
    ~TemporaryDatabase() {
        // Recebe uma possível falha sem lançar no destrutor.
        std::error_code ignored;
        // Tenta remover o arquivo temporário.
        std::filesystem::remove(path_, ignored);
    }

    // Retorna o caminho sem copiá-lo.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    // Guarda o caminho completo do banco temporário.
    std::filesystem::path path_;
};

} // namespace

// Executa os testes do layout de registros.
int main() {
    // Evita repetir o namespace do modelo relacional.
    using namespace modb;
    // Evita repetir o namespace do armazenamento.
    using namespace modb::storage;

    // Acumula e mostra as falhas encontradas.
    TestSuite suite;
    // Cria uma página vazia e formatada.
    auto page = SlottedPage::create();
    // Confere o estado inicial do diretório.
    suite.check(page.record_count() == 0, "new slotted page has no records");
    // O espaço inicial desconta somente o cabeçalho fixo atual.
    suite.check(page.free_space() == page_size - slotted_page_header_size,
                "new slotted page exposes its free space");
    suite.check(!page.previous_page() && !page.next_page(),
                "new slotted page starts with no neighbours");
    suite.check(page.set_previous_page(PageId{7}).has_value() &&
                    page.set_next_page(PageId{9}).has_value() &&
                    page.previous_page() == std::optional<PageId>{PageId{7}} &&
                    page.next_page() == std::optional<PageId>{PageId{9}},
                "slotted page persists both chain directions");
    suite.check(page.set_previous_page(std::nullopt).has_value() &&
                    page.set_next_page(std::nullopt).has_value(),
                "slotted page clears both chain directions");

    // Cria a primeira linha que será transformada em registro.
    const Row ana{Value{std::int64_t{1}}, Value{"Ana"}};
    // Codifica a linha para obter bytes de tamanho variável.
    auto encoded_ana = encode_row(ana);
    suite.check(encoded_ana.has_value(), "first Row is encoded");
    // Insere os bytes da primeira linha.
    auto ana_slot = page.insert(encoded_ana ? std::span<const std::byte>{*encoded_ana}
                                            : std::span<const std::byte>{});
    // O primeiro registro precisa receber o slot zero.
    suite.check(ana_slot == Result<SlotId>{SlotId{0}}, "first record receives slot zero");

    // Cria uma segunda linha com texto de tamanho diferente.
    const Row bia{Value{std::int64_t{2}}, Value{"Beatriz"}};
    // Codifica a segunda linha.
    auto encoded_bia = encode_row(bia);
    suite.check(encoded_bia.has_value(), "second Row is encoded");
    // Insere os bytes da segunda linha.
    auto bia_slot = page.insert(encoded_bia ? std::span<const std::byte>{*encoded_bia}
                                            : std::span<const std::byte>{});
    // O segundo registro precisa receber o slot um.
    suite.check(bia_slot == Result<SlotId>{SlotId{1}}, "second record receives slot one");
    // A página agora possui dois registros.
    suite.check(page.record_count() == 2, "record count grows after insertions");
    // Obtém a visão descritiva do diretório.
    const auto slots = page.slots();
    // Cada registro precisa possuir uma entrada correspondente.
    suite.check(slots.size() == 2, "layout exposes one SlotInfo per record");
    // O primeiro slot precisa apontar para todos os bytes da primeira Row.
    if (slots.size() == 2 && encoded_ana && encoded_bia) {
        suite.check(slots[0].id == SlotId{0} && slots[0].record_size == encoded_ana->size(),
                    "first SlotInfo exposes id and record size");
        suite.check(slots[1].id == SlotId{1} && slots[1].record_size == encoded_bia->size(),
                    "second SlotInfo exposes id and record size");
        suite.check(page.free_start() == slotted_page_header_size +
                                             page.record_count() * slotted_page_slot_size,
                    "free_start follows the slot directory");
        suite.check(page.free_end() == slots[1].record_offset,
                    "free_end points to the most recently inserted record");
    }

    // Lê novamente os bytes do primeiro slot.
    auto stored_ana = page.read(SlotId{0});
    suite.check(stored_ana.has_value(), "first slot can be read");
    // Decodifica os bytes recuperados.
    if (stored_ana) {
        auto decoded = decode_row(*stored_ana);
        suite.check(decoded.has_value() && *decoded == ana,
                    "first Row survives insertion and reading");
    }
    // Confere o erro produzido para um índice inexistente.
    suite.check_error(page.read(SlotId{99}), ErrorCode::slot_not_found,
                      "unknown slot is rejected");
    // Confere que um registro sem bytes não é aceito.
    suite.check_error(page.insert({}), ErrorCode::invalid_argument,
                      "empty record is rejected");

    // Copia os bytes para simular leitura a partir de outra camada.
    const Page raw_page = page.page();
    // Reconstrói a estrutura somente depois de validar o layout.
    auto reconstructed = SlottedPage::from_page(raw_page);
    suite.check(reconstructed.has_value(), "valid slotted page is reconstructed");
    if (reconstructed) {
        auto stored_bia = reconstructed->read(SlotId{1});
        suite.check(stored_bia.has_value(), "second slot survives reconstruction");
        if (stored_bia) {
            auto decoded_bia = decode_row(*stored_bia);
            suite.check(decoded_bia.has_value() && *decoded_bia == bia,
                        "second Row survives page reconstruction");
        }
    }

    // Altera a assinatura para simular outro tipo de página.
    auto bad_magic = raw_page;
    bad_magic[0] = std::byte{'X'};
    suite.check_error(SlottedPage::from_page(bad_magic), ErrorCode::invalid_page_format,
                      "invalid slotted page magic is rejected");
    // Altera o free_start armazenado no byte oito.
    auto bad_directory = raw_page;
    bad_directory[8] = std::byte{0U};
    bad_directory[9] = std::byte{0U};
    suite.check_error(SlottedPage::from_page(bad_directory), ErrorCode::corrupt_page,
                      "corrupt slot directory is rejected");

    // Cria outra página para testar a condição normal de falta de espaço.
    auto full_page = SlottedPage::create();
    // Um bloco grande cabe apenas uma vez e deixa pouco espaço livre.
    const std::vector<std::byte> large_record(page_size - 100, std::byte{0x2aU});
    suite.check(full_page.insert(large_record).has_value(), "large record fits once");
    // A segunda inserção precisa falhar sem alterar a contagem.
    suite.check_error(full_page.insert(large_record), ErrorCode::page_full,
                      "full page rejects another record");
    suite.check(full_page.record_count() == 1, "failed insertion does not create a slot");

    // Cria uma página isolada para exercitar capacidade, remoção e reutilização.
    auto reusable_page = SlottedPage::create();
    // Dez bytes devem ocupar exatamente dez bytes.
    const std::vector<std::byte> small_record(10, std::byte{0x11U});
    auto reusable_slot = reusable_page.insert(small_record);
    suite.check(reusable_slot == Result<SlotId>{SlotId{0}}, "small record uses slot zero");
    auto reusable_slots = reusable_page.slots();
    suite.check(reusable_slots[0].record_size == 10 &&
                    reusable_slots[0].record_capacity == 10 &&
                    reusable_slots[0].generation == 1,
                "small record uses its exact size with generation one");

    // Um crescimento com espaço na página deve preservar slot e geração.
    const auto original_offset = reusable_slots[0].record_offset;
    const std::vector<std::byte> fitting_update(50, std::byte{0x22U});
    suite.check(reusable_page.update(SlotId{0}, fitting_update).has_value(),
                "record grows inside its reserved capacity");
    reusable_slots = reusable_page.slots();
    suite.check(reusable_slots[0].record_offset != original_offset &&
                    reusable_slots[0].record_capacity == 50 &&
                    reusable_slots[0].generation == 1,
                "growing update reallocates exact capacity and preserves generation");

    // Outro slot permite verificar a compactação quando o primeiro for removido.
    const std::vector<std::byte> neighbour(70, std::byte{0x33U});
    suite.check(reusable_page.insert(neighbour) == Result<SlotId>{SlotId{1}},
                "second record uses slot one");
    const auto free_before_erase = reusable_page.free_space();
    suite.check(reusable_page.erase(SlotId{0}).has_value(), "first record is erased");
    suite.check(reusable_page.record_count() == 1 && reusable_page.slot_count() == 2,
                "erase keeps the free slot but reduces live record count");
    // Compactação preguiçosa: o erase NÃO recupera o espaço na hora — o buraco
    // (50 bytes) permanece e o espaço livre contíguo fica inalterado...
    suite.check(reusable_page.free_space() == free_before_erase,
                "lazy erase leaves the freed space as a hole, not reclaimed yet");
    // ...mas a capacidade é contabilizada como recuperável (uma inserção que
    // precise dela compacta sob demanda). Como o slot removido é reutilizável,
    // insertion_capacity soma o buraco sem descontar espaço de diretório.
    suite.check(reusable_page.insertion_capacity() == free_before_erase + 50,
                "the erased capacity is reported as reclaimable");

    // Uma nova ocupação reutiliza a entrada removida e avança sua geração.
    const std::vector<std::byte> replacement(20, std::byte{0x44U});
    suite.check(reusable_page.insert(replacement) == Result<SlotId>{SlotId{0}},
                "insertion reuses the removed SlotId");
    reusable_slots = reusable_page.slots();
    suite.check(reusable_slots[0].record_capacity == 20 &&
                    reusable_slots[0].generation == 2 && reusable_page.slot_count() == 2,
                "reused slot receives exact capacity and a new generation");
    suite.check(SlottedPage::from_page(reusable_page.page()).has_value(),
                "page remains valid after erase, compaction and reuse");

    // Um slot cuja geração se esgota deve ser aposentado para não revalidar IDs antigos.
    auto generation_page = SlottedPage::create();
    const std::vector<std::byte> generation_record(1, std::byte{0x55U});
    suite.check(generation_page.insert(generation_record) == Result<SlotId>{SlotId{0}},
                "generation exhaustion starts at slot zero");
    for (std::uint32_t generation = 2;
         generation <= std::numeric_limits<std::uint16_t>::max(); ++generation) {
        suite.check(generation_page.erase(SlotId{0}).has_value(),
                    "slot is erased while advancing its generation");
        auto reused = generation_page.insert(generation_record);
        if (!reused || *reused != SlotId{0}) {
            suite.check(false, "slot remains reusable before generation exhaustion");
            break;
        }
    }
    suite.check(generation_page.generation(SlotId{0}) ==
                    Result<std::uint16_t>{std::numeric_limits<std::uint16_t>::max()},
                "slot reaches the maximum generation");
    suite.check(generation_page.erase(SlotId{0}).has_value(),
                "slot at maximum generation can be erased");
    suite.check(generation_page.insert(generation_record) == Result<SlotId>{SlotId{1}},
                "exhausted slot is retired instead of wrapping to generation one");
    const auto exhausted_slots = generation_page.slots();
    suite.check(exhausted_slots.size() == 2 && !exhausted_slots[0].occupied() &&
                    exhausted_slots[0].generation ==
                        std::numeric_limits<std::uint16_t>::max() &&
                    exhausted_slots[1].generation == 1,
                "retired slot preserves its terminal generation");

    // Compactação sob demanda: uma inserção que não cabe no espaço livre
    // contíguo, mas caberia recuperando buracos, dispara a compactação e sucede.
    {
        SlottedPage fragmented = SlottedPage::create();
        const std::vector<std::byte> big_a(2000, std::byte{0x51U});
        const std::vector<std::byte> big_b(2000, std::byte{0x52U});
        suite.check(fragmented.insert(big_a) == Result<SlotId>{SlotId{0}}, "big A occupies slot 0");
        suite.check(fragmented.insert(big_b) == Result<SlotId>{SlotId{1}}, "big B occupies slot 1");
        const auto tight_free = fragmented.free_space();
        // Remove A: abre um buraco de 2000 bytes que não é recuperado na hora.
        suite.check(fragmented.erase(SlotId{0}).has_value(), "big A is erased, leaving a hole");
        suite.check(fragmented.free_space() == tight_free,
                    "the contiguous free space stays tight after the lazy erase");
        // Um registro de 1500 não cabe nos ~poucos bytes contíguos, mas cabe
        // depois de recuperar o buraco de 2000: a inserção compacta e sucede.
        const std::vector<std::byte> mid(1500, std::byte{0x53U});
        suite.check(fragmented.free_space() < mid.size(),
                    "the record does not fit the contiguous free space");
        suite.check(fragmented.insert(mid) == Result<SlotId>{SlotId{0}},
                    "on-demand compaction reclaims the hole and reuses slot 0");
        auto read_back = fragmented.read(SlotId{0});
        suite.check(read_back.has_value() && read_back->size() == 1500,
                    "the compacted record reads back at its full size");
        suite.check(SlottedPage::from_page(fragmented.page()).has_value(),
                    "the page validates after on-demand compaction");
    }

    // Prepara um arquivo temporário para o round-trip persistente.
    TemporaryDatabase database;
    std::cout << "Temporary slotted-page database: " << database.path() << '\n';
    // Cria, aloca e grava a página em uma primeira abertura.
    {
        auto file = PageFile::create(database.path());
        suite.check(file.has_value(), "persistence database is created");
        if (file) {
            auto page_id = file->allocate_page();
            suite.check(page_id == Result<PageId>{PageId{1}}, "record page receives PageId one");
            if (page_id) {
                suite.check(file->write(*page_id, raw_page).has_value(),
                            "slotted page is written through PageFile");
            }
            suite.check(file->flush().has_value(), "slotted page write is flushed");
        }
    }
    // Reabre o arquivo e reconstrói a slotted page persistida.
    {
        auto file = PageFile::open(database.path());
        suite.check(file.has_value(), "persistence database is reopened");
        if (file) {
            auto stored_page = file->read(PageId{1});
            suite.check(stored_page.has_value(), "persisted record page is read");
            if (stored_page) {
                auto stored_slotted = SlottedPage::from_page(std::move(*stored_page));
                suite.check(stored_slotted.has_value(), "persisted slotted page is valid");
                if (stored_slotted) {
                    auto record = stored_slotted->read(SlotId{0});
                    suite.check(record.has_value(), "persisted slot is read");
                    if (record) {
                        auto decoded = decode_row(*record);
                        suite.check(decoded.has_value() && *decoded == ana,
                                    "Row survives PageFile close and reopen");
                    }
                }
            }
        }
    }

    // Encerra o processo com o resultado acumulado.
    return suite.finish();
}
