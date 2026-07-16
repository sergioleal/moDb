// Importa SlottedPage, SlotId e RecordId.
#include "modb/storage/slotted_page.hpp"
// Importa store_le/load_le, a implementação única de little-endian.
#include "modb/storage/endian.hpp"

// Disponibiliza algoritmos para copiar, comparar e ordenar.
#include <algorithm>
// Disponibiliza blocos fixos usados pela assinatura.
#include <array>
// Disponibiliza std::to_integer.
#include <cstddef>
// Disponibiliza inteiros com largura fixa.
#include <cstdint>
// Disponibiliza o maior valor de uint16_t.
#include <limits>
// Disponibiliza std::to_string para mensagens de erro.
#include <string>
// Disponibiliza pares usados durante a validação dos intervalos.
#include <utility>
// Disponibiliza a lista temporária de intervalos dos registros.
#include <vector>

namespace modb::storage {
namespace {

// Identifica uma página formatada como slotted page.
constexpr std::array<std::byte, 4> magic{
    std::byte{'S'},
    std::byte{'L'},
    std::byte{'P'},
    std::byte{'G'},
};
// Usa localmente a versão pública que também aparece nas ferramentas de inspeção.
constexpr auto format_version = slotted_page_format_version;
// A versão ocupa o byte quatro.
constexpr std::size_t version_offset = 4;
// A quantidade de slots começa no byte seis.
constexpr std::size_t slot_count_offset = 6;
// O início do espaço livre começa no byte oito.
constexpr std::size_t free_start_offset = 8;
// O fim do espaço livre começa no byte dez.
constexpr std::size_t free_end_offset = 10;
// A ligação para a próxima página começa no byte doze.
constexpr std::size_t next_page_offset = 12;
// A ligação para a página anterior começa no byte vinte.
constexpr std::size_t previous_page_offset = 20;
// Usa localmente o tamanho público do cabeçalho.
constexpr auto header_size = slotted_page_header_size;
// Usa localmente o tamanho público de cada entrada.
constexpr auto slot_size = slotted_page_slot_size;

// Grava um uint16_t em little-endian dentro da página.
void write_u16(Page& page, std::size_t offset, std::uint16_t value) {
    // Delega à implementação única de little-endian sobre os bytes da página.
    store_le(page.bytes().subspan(offset, sizeof(value)), value);
}

// Lê um uint16_t little-endian da página.
std::uint16_t read_u16(const Page& page, std::size_t offset) {
    // Delega à implementação única de little-endian sobre os bytes da página.
    return load_le<std::uint16_t>(page.bytes().subspan(offset, sizeof(std::uint16_t)));
}

// Grava um uint64_t em little-endian dentro da página.
void write_u64(Page& page, std::size_t offset, std::uint64_t value) {
    // Delega à implementação única de little-endian sobre os bytes da página.
    store_le(page.bytes().subspan(offset, sizeof(value)), value);
}

// Lê um uint64_t little-endian da página.
std::uint64_t read_u64(const Page& page, std::size_t offset) {
    // Delega à implementação única de little-endian sobre os bytes da página.
    return load_le<std::uint64_t>(page.bytes().subspan(offset, sizeof(std::uint64_t)));
}

// Calcula onde começa a entrada de um slot.
std::size_t slot_offset(SlotId slot) {
    // O diretório começa depois do cabeçalho e cresce oito bytes por slot.
    return header_size + static_cast<std::size_t>(slot.value) * slot_size;
}

// Lê o offset do registro apontado pelo slot.
std::uint16_t record_offset(const Page& page, SlotId slot) {
    // O primeiro uint16_t da entrada é o offset.
    return read_u16(page, slot_offset(slot));
}

// Lê o tamanho do registro apontado pelo slot.
std::uint16_t record_size(const Page& page, SlotId slot) {
    // O segundo uint16_t da entrada é o tamanho.
    return read_u16(page, slot_offset(slot) + sizeof(std::uint16_t));
}

// Lê a capacidade reservada para o registro.
std::uint16_t record_capacity(const Page& page, SlotId slot) {
    // O terceiro uint16_t da entrada é a capacidade.
    return read_u16(page, slot_offset(slot) + 2 * sizeof(std::uint16_t));
}

// Lê a geração que protege o SlotId contra reutilização indevida.
std::uint16_t slot_generation(const Page& page, SlotId slot) {
    // O quarto uint16_t da entrada é a geração.
    return read_u16(page, slot_offset(slot) + 3 * sizeof(std::uint16_t));
}

// Grava todos os campos persistidos de uma entrada de slot.
void write_slot(Page& page, SlotId slot, std::uint16_t offset, std::uint16_t size,
                std::uint16_t capacity, std::uint16_t generation) {
    // Localiza o início da entrada.
    const auto entry = slot_offset(slot);
    // Persiste posição, tamanho lógico, capacidade e geração em little-endian.
    write_u16(page, entry, offset);
    write_u16(page, entry + sizeof(std::uint16_t), size);
    write_u16(page, entry + 2 * sizeof(std::uint16_t), capacity);
    write_u16(page, entry + 3 * sizeof(std::uint16_t), generation);
}

// Soma os bytes "mortos": capacidade ocupada por buracos de remoções que ainda
// não foram recuperados por uma compactação. É a região [free_end, page_size)
// menos a capacidade dos registros vivos. O(slots), no mesmo custo que a
// varredura de slot já feita por insert/insertion_capacity.
std::size_t dead_bytes(const Page& page) {
    const auto count = read_u16(page, slot_count_offset);
    std::size_t live_capacity = 0;
    for (std::uint16_t index = 0; index < count; ++index) {
        const SlotId slot{index};
        if (record_size(page, slot) != 0) {
            live_capacity += record_capacity(page, slot);
        }
    }
    const auto allocated = page_size - static_cast<std::size_t>(read_u16(page, free_end_offset));
    return allocated - live_capacity;
}

// Valida cabeçalho, diretório e intervalos dos registros.
Result<void> validate_page(const Page& page) {
    // Confere a assinatura antes de interpretar os outros campos.
    if (!std::equal(magic.begin(), magic.end(), page.bytes().begin())) {
        return std::unexpected(
            Error{ErrorCode::invalid_page_format, "page is not formatted as a slotted page"});
    }
    // Lê a versão armazenada diretamente em um byte.
    const auto version = std::to_integer<std::uint8_t>(page[version_offset]);
    // Aceita a versão atual e a anterior (v3 é um subconjunto sempre-compacto,
    // válido sob as regras relaxadas da v4).
    if (version != format_version && version != 3) {
        return std::unexpected(Error{
            ErrorCode::incompatible_page_version,
            "unsupported slotted page version: " + std::to_string(version),
        });
    }

    // Lê a quantidade e as duas fronteiras do espaço livre.
    const auto count = read_u16(page, slot_count_offset);
    const auto free_start = read_u16(page, free_start_offset);
    const auto free_end = read_u16(page, free_end_offset);
    // O início é determinado pela quantidade total de entradas do diretório.
    const auto expected_free_start = header_size + static_cast<std::size_t>(count) * slot_size;
    // Rejeita diretórios que ultrapassam a página ou possuem fronteira incorreta.
    if (expected_free_start > page_size || free_start != expected_free_start) {
        return std::unexpected(
            Error{ErrorCode::corrupt_page, "slotted page has an invalid slot directory"});
    }
    // O espaço livre nunca pode ter fronteiras invertidas ou sair da página.
    if (free_end < free_start || free_end > page_size) {
        return std::unexpected(
            Error{ErrorCode::corrupt_page, "slotted page has invalid free-space boundaries"});
    }

    // Guarda pares de offset e fim para detectar lacunas e sobreposições.
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    // Reserva exatamente a quantidade declarada.
    ranges.reserve(count);
    // Examina cada entrada do diretório.
    for (std::uint16_t index = 0; index < count; ++index) {
        // Cria o identificador correspondente ao índice atual.
        const SlotId slot{index};
        // Lê posição, tamanho lógico, capacidade física e geração.
        const auto offset = static_cast<std::size_t>(record_offset(page, slot));
        const auto size = static_cast<std::size_t>(record_size(page, slot));
        const auto capacity = static_cast<std::size_t>(record_capacity(page, slot));
        const auto generation = slot_generation(page, slot);
        // Uma geração zero nunca representa uma ocupação válida.
        if (generation == 0) {
            return std::unexpected(
                Error{ErrorCode::corrupt_page, "slotted page contains generation zero"});
        }
        // Um slot livre zera toda a descrição física, mas preserva sua geração.
        if (size == 0) {
            if (offset != 0 || capacity != 0) {
                return std::unexpected(Error{
                    ErrorCode::corrupt_page,
                    "free slot contains a record offset or capacity",
                });
            }
            continue;
        }
        // Todo registro vivo precisa respeitar sua capacidade reservada.
        if (capacity < size) {
            return std::unexpected(Error{
                ErrorCode::corrupt_page,
                "slotted page contains an invalid record capacity",
            });
        }
        // Evita overflow verificando a capacidade antes da soma.
        if (offset < free_end || offset > page_size || capacity > page_size - offset) {
            return std::unexpected(
                Error{ErrorCode::corrupt_page, "slotted page contains an invalid record range"});
        }
        // Guarda toda a área reservada, inclusive padding.
        ranges.emplace_back(offset, offset + capacity);
    }

    // Sem registros vivos a página é válida: o espaço livre e os buracos ainda
    // não recuperados ocupam o resto — não se exige mais free_end no fim físico.
    if (ranges.empty()) {
        return {};
    }

    // Ordena por posição física só para detectar sobreposição. Diferente do
    // layout sempre-compacto anterior, LACUNAS entre registros são permitidas:
    // são os buracos deixados por remoções, recuperados sob demanda na próxima
    // inserção (compactação preguiçosa). Os limites de cada registro (>= free_end
    // e dentro da página) já foram verificados acima.
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        if (ranges[index - 1].second > ranges[index].first) {
            return std::unexpected(
                Error{ErrorCode::corrupt_page, "slotted page record ranges overlap"});
        }
    }
    // Todas as invariantes foram verificadas.
    return {};
}

} // namespace

// Cria uma slotted page vazia com cabeçalho válido.
SlottedPage SlottedPage::create() {
    // Page começa preenchida com zeros.
    Page page;
    // Copia a assinatura SLPG para os primeiros bytes.
    std::copy(magic.begin(), magic.end(), page.bytes().begin());
    // Grava a versão atual em um byte.
    page[version_offset] = std::byte{format_version};
    // Uma página nova ainda não possui slots.
    write_u16(page, slot_count_offset, 0);
    // O diretório vazio termina logo após o cabeçalho.
    write_u16(page, free_start_offset, static_cast<std::uint16_t>(header_size));
    // Os registros começarão a crescer a partir do final da página.
    write_u16(page, free_end_offset, static_cast<std::uint16_t>(page_size));
    // Zero indica que ainda não existe próxima página.
    write_u64(page, next_page_offset, 0);
    // Zero indica que uma página isolada ainda não possui anterior.
    write_u64(page, previous_page_offset, 0);
    // Transfere a página formatada para a nova instância.
    return SlottedPage{std::move(page)};
}

// Valida bytes existentes antes de tratá-los como slotted page.
Result<SlottedPage> SlottedPage::from_page(Page page) {
    // Rejeita imediatamente qualquer invariante inválida.
    if (auto result = validate_page(page); !result) {
        return std::unexpected(result.error());
    }
    // Move os bytes validados para a instância retornada.
    return SlottedPage{std::move(page)};
}

// Reconstrói sem revalidar; o chamador garante que a página já é confiável.
SlottedPage SlottedPage::from_trusted_page(Page page) {
    // Evita o custo de validate_page (varredura, alocação e sort) por leitura.
    return SlottedPage{std::move(page)};
}

// Insere um registro no fim do espaço livre e cria seu slot.
Result<SlotId> SlottedPage::insert(std::span<const std::byte> record) {
    // Registros vazios não possuem significado neste layout.
    if (record.empty()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "record cannot be empty"});
    }
    // Distingue um registro impossível de uma página apenas ocupada.
    if (record.size() > slotted_page_max_record_size) {
        return std::unexpected(Error{
            ErrorCode::record_too_large,
            "record exceeds the maximum slotted-page payload",
        });
    }
    // Cada registro reserva exatamente o espaço ocupado por seu conteúdo.
    const auto capacity = record.size();
    // Procura primeiro uma entrada removida cuja geração ainda possa avançar.
    std::optional<SlotId> reusable;
    for (std::uint16_t index = 0; index < slot_count(); ++index) {
        const SlotId candidate{index};
        if (record_size(page_, candidate) == 0 &&
            slot_generation(page_, candidate) != std::numeric_limits<std::uint16_t>::max()) {
            reusable = candidate;
            break;
        }
    }
    // Uma nova entrada só é necessária quando não existe slot livre.
    const auto required = capacity + (reusable ? 0 : slot_size);
    // Se não cabe na região livre contígua, tenta recuperar os buracos de
    // remoções anteriores (compactação preguiçosa): compacta apenas quando o
    // espaço é de fato necessário, não a cada delete. Só então declara a página
    // cheia — quando nem os buracos recuperados dariam conta.
    if (required > free_space()) {
        if (required > free_space() + dead_bytes(page_)) {
            return std::unexpected(Error{ErrorCode::page_full, "slotted page is full"});
        }
        compact();
    }

    // Reutiliza a entrada livre ou acrescenta uma ao diretório.
    const SlotId slot = reusable.value_or(SlotId{slot_count()});
    // Lê a fronteira superior atual do espaço livre.
    const auto old_free_end = read_u16(page_, free_end_offset);
    // Reserva o espaço do registro crescendo do fim para o início.
    const auto new_free_end = static_cast<std::uint16_t>(old_free_end - capacity);
    // Copia todos os bytes do registro para a área reservada.
    std::copy(record.begin(), record.end(), page_.bytes().begin() + new_free_end);
    // Limpa o restante da capacidade para produzir bytes determinísticos.
    std::fill(page_.bytes().begin() + static_cast<std::ptrdiff_t>(new_free_end + record.size()),
              page_.bytes().begin() + static_cast<std::ptrdiff_t>(new_free_end + capacity),
              std::byte{0});

    // Uma reutilização avança a geração; um slot novo começa em um.
    const auto generation = reusable
                                ? static_cast<std::uint16_t>(slot_generation(page_, slot) + 1)
                                : std::uint16_t{1};
    // Persiste todos os metadados do registro.
    write_slot(page_, slot, new_free_end, static_cast<std::uint16_t>(record.size()),
               static_cast<std::uint16_t>(capacity), generation);
    // Um slot novo aumenta o diretório; uma reutilização mantém sua fronteira.
    if (!reusable) {
        write_u16(page_, slot_count_offset, static_cast<std::uint16_t>(slot.value + 1));
        write_u16(page_, free_start_offset,
                  static_cast<std::uint16_t>(slot_offset(slot) + slot_size));
    }
    // Atualiza o início físico da área de registros.
    write_u16(page_, free_end_offset, new_free_end);
    // Retorna o identificador estável criado para o registro.
    return slot;
}

// Copia os bytes pertencentes a um slot existente.
Result<std::vector<std::byte>> SlottedPage::read(SlotId slot) const {
    // Rejeita índices que ainda não foram criados.
    if (slot.value >= slot_count() || record_size(page_, slot) == 0) {
        return std::unexpected(Error{
            ErrorCode::slot_not_found,
            "slot does not exist: " + std::to_string(slot.value),
        });
    }
    // Obtém a posição inicial persistida no diretório.
    const auto offset = static_cast<std::size_t>(record_offset(page_, slot));
    // Obtém a quantidade de bytes persistida no diretório.
    const auto size = static_cast<std::size_t>(record_size(page_, slot));
    // Cria uma visão limitada exatamente ao intervalo validado do registro.
    const auto bytes = page_.bytes().subspan(offset, size);
    // Copia o intervalo para que o resultado não dependa da vida da página.
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

// Retorna a geração apenas de slots atualmente ocupados.
Result<std::uint16_t> SlottedPage::generation(SlotId slot) const {
    // Reutiliza as mesmas regras de existência da leitura.
    if (slot.value >= slot_count() || record_size(page_, slot) == 0) {
        return std::unexpected(
            Error{ErrorCode::slot_not_found, "slot does not exist: " + std::to_string(slot.value)});
    }
    return slot_generation(page_, slot);
}

// Substitui um registro sem alterar seu identificador lógico.
Result<void> SlottedPage::update(SlotId slot, std::span<const std::byte> record) {
    // Rejeita registros vazios e slots removidos ou inexistentes.
    if (record.empty()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "record cannot be empty"});
    }
    if (slot.value >= slot_count() || record_size(page_, slot) == 0) {
        return std::unexpected(
            Error{ErrorCode::slot_not_found, "slot does not exist: " + std::to_string(slot.value)});
    }
    // Impede um registro que nunca caberia em uma página.
    if (record.size() > slotted_page_max_record_size) {
        return std::unexpected(Error{
            ErrorCode::record_too_large,
            "record exceeds the maximum slotted-page payload",
        });
    }

    // Preserva os metadados estáveis do slot atual.
    auto offset = record_offset(page_, slot);
    const auto old_capacity = record_capacity(page_, slot);
    const auto generation = slot_generation(page_, slot);
    const auto new_capacity = record.size();
    // Qualquer mudança de tamanho recupera a área antiga e realoca o mesmo slot.
    if (new_capacity != old_capacity) {
        // Após liberar este slot e compactar, o espaço livre será a região livre
        // atual + todos os buracos + a capacidade antiga deste registro.
        if (new_capacity > free_space() + dead_bytes(page_) + old_capacity) {
            return std::unexpected(Error{ErrorCode::page_full, "slotted page is full"});
        }
        // Retira temporariamente o intervalo antigo antes da compactação.
        write_slot(page_, slot, 0, 0, 0, generation);
        compact();
        // Reserva uma nova área maior no início físico dos registros.
        offset = static_cast<std::uint16_t>(free_end() - new_capacity);
        write_u16(page_, free_end_offset, offset);
    }
    // Copia o novo conteúdo para sua área de tamanho exato.
    std::copy(record.begin(), record.end(), page_.bytes().begin() + offset);
    // Mantém geração e SlotId porque o registro lógico continua sendo o mesmo.
    write_slot(page_, slot, offset, static_cast<std::uint16_t>(record.size()),
               static_cast<std::uint16_t>(new_capacity), generation);
    return {};
}

// Remove um registro e recupera sua capacidade para o espaço livre central.
Result<void> SlottedPage::erase(SlotId slot) {
    // Um slot livre é tratado como inexistente.
    if (slot.value >= slot_count() || record_size(page_, slot) == 0) {
        return std::unexpected(
            Error{ErrorCode::slot_not_found, "slot does not exist: " + std::to_string(slot.value)});
    }
    // Preserva a geração para que a próxima ocupação possa avançá-la.
    const auto generation = slot_generation(page_, slot);
    // Zera a descrição física e mantém a entrada disponível. O espaço vira um
    // buraco "morto" recuperado sob demanda na próxima inserção (compactação
    // preguiçosa) — erase é O(1), sem compactar a cada remoção.
    write_slot(page_, slot, 0, 0, 0, generation);
#ifdef MODB_EAGER_COMPACT
    // Toggle exclusivo de benchmark (não definido em builds normais): restaura a
    // compactação ansiosa para comparar contra a preguiçosa sob o mesmo padrão.
    compact();
#endif
    return {};
}

// Reagrupa capacidades ocupadas no final da página.
void SlottedPage::compact() {
    // Trabalha sobre uma cópia para que movimentos sobrepostos sejam seguros.
    const Page source = page_;
    // A primeira capacidade será posicionada no final físico.
    auto destination = page_size;
    // A ordem de SlotId é suficiente; offsets não fazem parte da identidade.
    for (std::uint16_t index = 0; index < slot_count(); ++index) {
        const SlotId slot{index};
        const auto size = record_size(source, slot);
        // Slots livres não possuem intervalo físico.
        if (size == 0) {
            continue;
        }
        const auto capacity = record_capacity(source, slot);
        destination -= capacity;
        const auto source_offset = record_offset(source, slot);

        if (source_offset != destination) {
            std::copy_n(source.bytes().begin() + source_offset, capacity,
                        page_.bytes().begin() + static_cast<std::ptrdiff_t>(destination));
            // Atualiza somente o offset e preserva os demais campos.
            write_slot(page_, slot, static_cast<std::uint16_t>(destination), size, capacity,
                       slot_generation(source, slot));
        }
    }
    // Tudo antes da primeira capacidade agrupada volta a ser espaço livre.
    write_u16(page_, free_end_offset, static_cast<std::uint16_t>(destination));
}

// Lê a quantidade persistida no cabeçalho.
std::uint16_t SlottedPage::record_count() const noexcept {
    // Conta somente slots que ainda possuem conteúdo.
    std::uint16_t count = 0;
    for (std::uint16_t index = 0; index < slot_count(); ++index) {
        if (record_size(page_, SlotId{index}) != 0) {
            ++count;
        }
    }
    return count;
}

// Lê a quantidade total de entradas persistida no cabeçalho.
std::uint16_t SlottedPage::slot_count() const noexcept {
    return read_u16(page_, slot_count_offset);
}

// Calcula a distância entre o diretório e a área de registros.
std::size_t SlottedPage::free_space() const noexcept {
    // Lê onde termina o diretório de slots.
    const auto free_start = read_u16(page_, free_start_offset);
    // Lê onde começam os registros.
    const auto free_end = read_u16(page_, free_end_offset);
    // from_page e create garantem que a subtração não fica negativa.
    return static_cast<std::size_t>(free_end - free_start);
}

// Calcula a capacidade útil de uma próxima inserção, incluindo seu slot quando necessário.
std::size_t SlottedPage::insertion_capacity() const noexcept {
    // Espaço recuperável = região livre contígua + buracos que uma compactação
    // sob demanda devolveria. Sem contar os buracos, o TableHeap acharia cheia
    // uma página que ainda comporta o registro após compactar.
    const auto reclaimable = free_space() + dead_bytes(page_);
    for (std::uint16_t index = 0; index < slot_count(); ++index) {
        const SlotId slot{index};
        if (record_size(page_, slot) == 0 &&
            slot_generation(page_, slot) != std::numeric_limits<std::uint16_t>::max()) {
            // Há slot reutilizável: a inserção não precisa crescer o diretório.
            return reclaimable;
        }
    }
    return reclaimable >= slot_size ? reclaimable - slot_size : 0;
}

// Lê a fronteira final do diretório de slots.
std::uint16_t SlottedPage::free_start() const noexcept {
    return read_u16(page_, free_start_offset);
}

// Lê a fronteira inicial da área de registros.
std::uint16_t SlottedPage::free_end() const noexcept {
    return read_u16(page_, free_end_offset);
}

// Constrói uma descrição dos slots sem expor os bytes para alteração.
std::vector<SlotInfo> SlottedPage::slots() const {
    // Reserva a quantidade exata para evitar realocações.
    std::vector<SlotInfo> result;
    result.reserve(slot_count());
    // Percorre as entradas na ordem estável de SlotId.
    for (std::uint16_t index = 0; index < slot_count(); ++index) {
        // Monta o identificador da entrada atual.
        const SlotId slot{index};
        // Copia todos os metadados persistidos no diretório.
        result.push_back(SlotInfo{slot, record_offset(page_, slot), record_size(page_, slot),
                                  record_capacity(page_, slot), slot_generation(page_, slot)});
    }
    // Entrega a lista de metadados ao chamador.
    return result;
}

// Lê a ligação persistida nos últimos oito bytes do cabeçalho.
std::optional<PageId> SlottedPage::next_page() const noexcept {
    // Zero é o sentinela porque a página zero pertence ao superbloco.
    const auto value = read_u64(page_, next_page_offset);
    if (value == 0) {
        return std::nullopt;
    }
    // Qualquer outro valor representa uma página da cadeia.
    return PageId{value};
}

// Atualiza a ligação para outra slotted page.
Result<void> SlottedPage::set_next_page(std::optional<PageId> next) {
    // A página zero é reservada e representa ausência de ligação.
    if (next && next->value == 0) {
        return std::unexpected(Error{
            ErrorCode::invalid_argument,
            "page zero cannot be the next record page",
        });
    }
    // Grava zero para fim da cadeia ou o PageId recebido.
    write_u64(page_, next_page_offset, next ? next->value : 0);
    // A ligação foi atualizada em memória.
    return {};
}

// Lê a ligação reversa persistida no cabeçalho.
std::optional<PageId> SlottedPage::previous_page() const noexcept {
    const auto value = read_u64(page_, previous_page_offset);
    if (value == 0) {
        return std::nullopt;
    }
    return PageId{value};
}

// Atualiza a ligação para a slotted page anterior.
Result<void> SlottedPage::set_previous_page(std::optional<PageId> previous) {
    if (previous && previous->value == 0) {
        return std::unexpected(Error{
            ErrorCode::invalid_argument,
            "page zero cannot be the previous record page",
        });
    }
    write_u64(page_, previous_page_offset, previous ? previous->value : 0);
    return {};
}

} // namespace modb::storage
