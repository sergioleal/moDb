// Importa TableHeap e suas estruturas auxiliares.
#include "modb/storage/table_heap.hpp"
#include "modb/storage/binary.hpp"

// Disponibiliza algoritmos e blocos fixos usados pela assinatura.
#include <algorithm>
#include <array>
// Disponibiliza std::to_string para mensagens de ciclo.
#include <string>
// Disponibiliza o conjunto usado para detectar ciclos.
#include <unordered_set>
// Disponibiliza std::move.
#include <utility>

namespace modb::storage {
namespace {

// Identifica a página dedicada de metadados do TableHeap.
constexpr std::array<std::byte, 4> root_magic{
    std::byte{'T'}, std::byte{'H'}, std::byte{'R'}, std::byte{'P'}};
constexpr std::uint16_t root_format_version = 1;

// Representa os campos persistidos na raiz sem expor o formato binário.
struct RootMetadata {
    std::optional<PageId> first;
    std::optional<PageId> last;
    std::uint64_t page_count{};
    std::uint64_t record_count{};
};

// Converte zero no sentinela de ausência usado pelo formato.
std::optional<PageId> optional_page_id(std::uint64_t value) {
    return value == 0 ? std::nullopt : std::optional<PageId>{PageId{value}};
}

// Produz uma página THRP preenchida com os metadados atuais.
Page encode_root(const RootMetadata& metadata) {
    BinaryWriter writer;
    writer.write_bytes(root_magic);
    writer.write_u16(root_format_version);
    writer.write_u16(0);
    writer.write_u64(metadata.first ? metadata.first->value : 0);
    writer.write_u64(metadata.last ? metadata.last->value : 0);
    writer.write_u64(metadata.page_count);
    writer.write_u64(metadata.record_count);

    Page page;
    std::copy(writer.bytes().begin(), writer.bytes().end(), page.bytes().begin());
    return page;
}

// Valida e decodifica uma página dedicada de metadados.
Result<RootMetadata> decode_root(const Page& page) {
    BinaryReader reader{page.bytes()};
    auto magic = reader.read_bytes(root_magic.size());
    if (!magic || !std::equal(root_magic.begin(), root_magic.end(), magic->begin())) {
        return std::unexpected(
            Error{ErrorCode::invalid_page_format, "page is not a TableHeap root"});
    }
    auto version = reader.read_u16();
    auto flags = reader.read_u16();
    auto first = reader.read_u64();
    auto last = reader.read_u64();
    auto page_count = reader.read_u64();
    auto record_count = reader.read_u64();
    if (!version || !flags || !first || !last || !page_count || !record_count) {
        return std::unexpected(
            Error{ErrorCode::corrupt_page, "TableHeap root metadata is truncated"});
    }
    if (*version != root_format_version) {
        return std::unexpected(Error{
            ErrorCode::incompatible_page_version,
            "unsupported TableHeap root version: " + std::to_string(*version),
        });
    }
    if (*flags != 0) {
        return std::unexpected(
            Error{ErrorCode::corrupt_page, "TableHeap root contains unknown flags"});
    }

    RootMetadata metadata{optional_page_id(*first), optional_page_id(*last),
                          *page_count, *record_count};
    const auto empty = metadata.page_count == 0;
    if (metadata.first.has_value() != metadata.last.has_value() ||
        empty != !metadata.first.has_value() || (empty && metadata.record_count != 0)) {
        return std::unexpected(
            Error{ErrorCode::corrupt_page, "TableHeap root metadata is inconsistent"});
    }
    return metadata;
}

} // namespace

// Expõe a validação da raiz para inventários e o db check.
Result<void> validate_table_heap_root(const Page& page) {
    auto metadata = decode_root(page);
    if (!metadata) {
        return std::unexpected(metadata.error());
    }
    return {};
}

namespace {

// Registra uma visita e retorna erro quando a página já apareceu na cadeia.
Result<void> visit_page(std::unordered_set<std::uint64_t>& visited, PageId id) {
    // insert retorna false quando o mesmo PageId já estava no conjunto.
    if (!visited.insert(id.value).second) {
        return std::unexpected(Error{
            ErrorCode::page_chain_cycle,
            "TableHeap page chain contains a cycle at page " + std::to_string(id.value),
        });
    }
    // A página foi visitada pela primeira vez.
    return {};
}

} // namespace

// Reconstrói a raiz a partir da cadeia, sem confiar nos contadores persistidos.
Result<TableHeapRepairReport> repair_table_heap(PageFile& file, PageId root) {
    // A página zero pertence ao superbloco e nunca é raiz de heap.
    if (root.value == 0) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "TableHeap root cannot be page zero"});
    }
    // Lê e decodifica a raiz apenas estruturalmente, sem validar contra a cadeia.
    auto root_page = file.read(root);
    if (!root_page) {
        return std::unexpected(root_page.error());
    }
    auto metadata = decode_root(*root_page);
    if (!metadata) {
        return std::unexpected(metadata.error());
    }

    // Recalcula extremos e contadores percorrendo a cadeia de páginas de dados.
    RootMetadata rebuilt{};
    if (metadata->first) {
        std::unordered_set<std::uint64_t> visited;
        auto current_id = *metadata->first;
        std::uint64_t pages = 0;
        std::uint64_t records = 0;
        std::optional<PageId> last;
        while (true) {
            // Um ciclo é corrupção que o reparo não consegue resolver com segurança.
            if (auto visit = visit_page(visited, current_id); !visit) {
                return std::unexpected(visit.error());
            }
            // Cada página de dados precisa ter layout válido para ser contada.
            auto page = file.read(current_id);
            if (!page) {
                return std::unexpected(page.error());
            }
            auto slotted = SlottedPage::from_page(*page);
            if (!slotted) {
                return std::unexpected(slotted.error());
            }
            ++pages;
            records += slotted->record_count();
            last = current_id;
            // Avança pela ligação direta, a parte autodescritiva da cadeia.
            const auto next = slotted->next_page();
            if (!next) {
                break;
            }
            current_id = *next;
        }
        rebuilt = RootMetadata{metadata->first, last, pages, records};
    }

    // Só reescreve a raiz quando algum campo realmente mudou.
    const bool changed = rebuilt.first != metadata->first || rebuilt.last != metadata->last ||
                         rebuilt.page_count != metadata->page_count ||
                         rebuilt.record_count != metadata->record_count;
    if (changed) {
        if (auto written = file.write(root, encode_root(rebuilt)); !written) {
            return std::unexpected(written.error());
        }
    }
    return TableHeapRepairReport{rebuilt.page_count, rebuilt.record_count, changed};
}

// Cria a raiz dedicada de um novo heap vazio.
Result<TableHeap> TableHeap::create(PageFile& file, std::size_t scratch_page_count) {
    // A fábrica valida a capacidade num único ponto e sem lançar exceções.
    auto pool = ScratchPagePool::create(scratch_page_count);
    if (!pool) {
        return std::unexpected(pool.error());
    }
    // Reserva uma nova página física no arquivo.
    auto root = file.allocate_page();
    // Propaga uma falha de alocação.
    if (!root) {
        return std::unexpected(root.error());
    }
    // A raiz nasce sem páginas de dados nem registros.
    const auto page = encode_root(RootMetadata{});
    if (auto written = file.write(*root, page); !written) {
        return std::unexpected(written.error());
    }
    // Retorna o heap associado ao arquivo fornecido.
    return TableHeap{file, *root, std::move(*pool)};
}

// Abre um heap somente depois de validar toda a cadeia.
Result<TableHeap> TableHeap::open(PageFile& file, PageId root,
                                  std::size_t scratch_page_count) {
    // A fábrica valida a capacidade num único ponto e sem lançar exceções.
    auto pool = ScratchPagePool::create(scratch_page_count);
    if (!pool) {
        return std::unexpected(pool.error());
    }
    // A página zero pertence ao superbloco e nunca pode ser raiz de heap.
    if (root.value == 0) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "TableHeap root cannot be page zero"});
    }
    auto root_page = file.read(root);
    if (!root_page) {
        return std::unexpected(root_page.error());
    }
    auto metadata = decode_root(*root_page);
    if (!metadata) {
        return std::unexpected(metadata.error());
    }
    // Monta o objeto com o estado persistido para validar toda a cadeia.
    TableHeap heap{file, root, std::move(*pool), metadata->first, metadata->last,
                   metadata->page_count, metadata->record_count};
    // Percorre assinatura, versões, ligações e ciclos antes de retornar.
    if (auto pages = heap.layout(); !pages) {
        return std::unexpected(pages.error());
    }
    // A cadeia inteira é válida.
    return heap;
}

// Grava os metadados espelhados em memória na raiz dedicada.
Result<void> TableHeap::persist_root() {
    return file_->write(root_, encode_root(RootMetadata{
                                   first_, last_, page_count_, record_count_}));
}

// Lê uma página física e valida seu layout de registros.
Result<SlottedPage> TableHeap::load(PageId id) {
    // Empresta uma área previamente alocada e lê diretamente sobre ela.
    auto scratch = scratch_page_pool_->try_acquire();
    // Em uso single-thread com capacidade fixa isto nunca falta; reporta erro
    // em vez de bloquear ou desreferenciar um buffer inexistente.
    if (!scratch) {
        return std::unexpected(
            Error{ErrorCode::io_error, "no scratch page buffer available for read"});
    }
    auto read = file_->read(id, scratch->get());
    // Propaga PageId inexistente ou falha de I/O.
    if (!read) {
        return std::unexpected(read.error());
    }
    // SlottedPage mantém sua cópia proprietária; o scratch volta ao pool ao sair.
    return SlottedPage::from_page(scratch->get());
}

// Lê uma página confiável sem repetir a validação estrutural completa.
Result<SlottedPage> TableHeap::load_trusted(PageId id) {
    // Reutiliza o mesmo buffer emprestado, evitando alocações por leitura.
    auto scratch = scratch_page_pool_->try_acquire();
    // Mesmo contrato do load: falta de buffer vira erro explícito, não bloqueio.
    if (!scratch) {
        return std::unexpected(
            Error{ErrorCode::io_error, "no scratch page buffer available for read"});
    }
    auto read = file_->read(id, scratch->get());
    if (!read) {
        return std::unexpected(read.error());
    }
    // A cadeia foi validada ao abrir e as escritas preservam as invariantes,
    // então pular validate_page aqui elimina o sort e a alocação por leitura.
    return SlottedPage::from_trusted_page(scratch->get());
}

// Insere um registro escolhendo ou criando a página necessária.
Result<RecordId> TableHeap::insert(std::span<const std::byte> record) {
    // Evita percorrer páginas para um registro que nunca poderá caber.
    if (record.size() > slotted_page_max_record_size) {
        return std::unexpected(Error{
            ErrorCode::record_too_large,
            "record exceeds the maximum TableHeap page payload",
        });
    }
    // A primeira inserção cria a primeira página de dados do heap vazio.
    if (!first_) {
        auto page_id = file_->allocate_page();
        if (!page_id) {
            return std::unexpected(page_id.error());
        }
        auto page = SlottedPage::create();
        auto slot = page.insert(record);
        if (!slot) {
            return std::unexpected(slot.error());
        }
        auto generation = page.generation(*slot);
        if (!generation) {
            return std::unexpected(generation.error());
        }
        if (auto written = file_->write(*page_id, page.page()); !written) {
            return std::unexpected(written.error());
        }
        first_ = *page_id;
        last_ = *page_id;
        page_count_ = 1;
        record_count_ = 1;
        page_ids_.insert(page_id->value);
        insertion_capacity_by_page_[page_id->value] = page.insertion_capacity();
        if (auto persisted = persist_root(); !persisted) {
            return std::unexpected(persisted.error());
        }
        return RecordId{*page_id, *slot, *generation};
    }

    // Tenta uma candidata e diferencia página cheia de uma falha real.
    const auto try_insert = [&](PageId page_id) -> Result<std::optional<RecordId>> {
        auto page = load_trusted(page_id);
        if (!page) {
            return std::unexpected(page.error());
        }
        auto slot = page->insert(record);
        if (!slot) {
            if (slot.error().code == ErrorCode::page_full) {
                insertion_capacity_by_page_[page_id.value] = page->insertion_capacity();
                return std::nullopt;
            }
            return std::unexpected(slot.error());
        }
        auto generation = page->generation(*slot);
        if (!generation) {
            return std::unexpected(generation.error());
        }
        if (auto written = file_->write(page_id, page->page()); !written) {
            return std::unexpected(written.error());
        }
        insertion_capacity_by_page_[page_id.value] = page->insertion_capacity();
        ++record_count_;
        if (auto persisted = persist_root(); !persisted) {
            return std::unexpected(persisted.error());
        }
        return std::optional<RecordId>{RecordId{page_id, *slot, *generation}};
    };

    // Coleta as candidatas antes de inserir para não mutar o mapa durante a
    // iteração; a ordem de std::map por PageId prioriza as páginas mais antigas
    // de forma determinística entre execuções e plataformas.
    std::vector<std::uint64_t> candidates;
    for (const auto& [page_value, insertion_capacity] : insertion_capacity_by_page_) {
        if ((last_ && page_value == last_->value) || insertion_capacity < record.size()) {
            continue;
        }
        candidates.push_back(page_value);
    }
    for (const auto page_value : candidates) {
        auto inserted = try_insert(PageId{page_value});
        if (!inserted) {
            return std::unexpected(inserted.error());
        }
        if (*inserted) {
            return **inserted;
        }
    }
    // A última página é a candidata normal quando não há buracos reaproveitáveis.
    if (last_) {
        auto inserted = try_insert(*last_);
        if (!inserted) {
            return std::unexpected(inserted.error());
        }
        if (*inserted) {
            return **inserted;
        }
    }

    // Nenhuma candidata serviu; acrescenta diretamente depois de last_page.
    if (!last_) {
        return std::unexpected(
            Error{ErrorCode::corrupt_page, "non-empty TableHeap has no last page"});
    }
    const auto previous_id = *last_;
    auto previous = load_trusted(previous_id);
    if (!previous) {
        return std::unexpected(previous.error());
    }
    if (previous->next_page()) {
        return std::unexpected(Error{
            ErrorCode::corrupt_page,
            "TableHeap last page unexpectedly has a next-page link",
        });
    }
    auto new_page_id = file_->allocate_page();
    if (!new_page_id) {
        return std::unexpected(new_page_id.error());
    }
    auto new_page = SlottedPage::create();
    if (auto linked = new_page.set_previous_page(previous_id); !linked) {
        return std::unexpected(linked.error());
    }
    auto new_slot = new_page.insert(record);
    if (!new_slot) {
        return std::unexpected(new_slot.error());
    }
    auto generation = new_page.generation(*new_slot);
    if (!generation) {
        return std::unexpected(generation.error());
    }
    if (auto written = file_->write(*new_page_id, new_page.page()); !written) {
        return std::unexpected(written.error());
    }
    if (auto linked = previous->set_next_page(*new_page_id); !linked) {
        return std::unexpected(linked.error());
    }
    if (auto written = file_->write(previous_id, previous->page()); !written) {
        return std::unexpected(written.error());
    }
    insertion_capacity_by_page_[previous_id.value] = previous->insertion_capacity();
    insertion_capacity_by_page_[new_page_id->value] = new_page.insertion_capacity();
    page_ids_.insert(new_page_id->value);
    last_ = *new_page_id;
    ++page_count_;
    ++record_count_;
    if (auto persisted = persist_root(); !persisted) {
        return std::unexpected(persisted.error());
    }
    return RecordId{*new_page_id, *new_slot, *generation};
}

// Lê um RecordId usando o índice de páginas validado ao abrir o heap.
Result<std::vector<std::byte>> TableHeap::read(RecordId id) {
    if (!page_ids_.contains(id.page.value)) {
        return std::unexpected(Error{
            ErrorCode::record_not_found,
            "record page does not belong to this TableHeap",
        });
    }
    auto current = load_trusted(id.page);
    if (!current) {
        return std::unexpected(current.error());
    }
    // Um SlotId reutilizado não pode satisfazer um RecordId antigo.
    auto generation = current->generation(id.slot);
    if (!generation || *generation != id.generation) {
        return std::unexpected(Error{
            ErrorCode::record_not_found,
            "record generation does not match the occupied slot",
        });
    }
    return current->read(id.slot);
}

// Produz todos os endereços na ordem lógica da cadeia em uma única passada.
Result<std::vector<RecordId>> TableHeap::scan() {
    // Acumula os endereços sem copiar os registros.
    std::vector<RecordId> records;
    // Um heap vazio não possui páginas de dados para percorrer.
    if (!first_) {
        return records;
    }
    // Detecta ciclos para que o percurso não fique infinito.
    std::unordered_set<std::uint64_t> visited;
    // Começa na primeira página persistida na raiz.
    auto current_id = *first_;
    while (true) {
        if (auto visit = visit_page(visited, current_id); !visit) {
            return std::unexpected(visit.error());
        }
        // Carrega a página exatamente uma vez (cadeia já validada ao abrir).
        auto current = load_trusted(current_id);
        if (!current) {
            return std::unexpected(current.error());
        }
        // Mantém a ordem estável das entradas do diretório.
        for (const auto& slot : current->slots()) {
            if (slot.occupied()) {
                records.push_back(RecordId{current_id, slot.id, slot.generation});
            }
        }
        // Encerra ao encontrar o sentinela de fim da cadeia.
        const auto next = current->next_page();
        if (!next) {
            return records;
        }
        // Avança para a página ligada.
        current_id = *next;
    }
}

// Produz endereços e bytes de todos os registros lendo cada página uma única vez.
Result<std::vector<HeapRecord>> TableHeap::scan_records() {
    // Acumula endereço e conteúdo de cada registro vivo.
    std::vector<HeapRecord> records;
    // Um heap vazio não possui páginas de dados.
    if (!first_) {
        return records;
    }
    // Detecta ciclos para que o percurso não fique infinito.
    std::unordered_set<std::uint64_t> visited;
    // Começa na primeira página persistida na raiz.
    auto current_id = *first_;
    while (true) {
        if (auto visit = visit_page(visited, current_id); !visit) {
            return std::unexpected(visit.error());
        }
        // Carrega a página uma única vez; os bytes saem da cópia em memória.
        auto current = load_trusted(current_id);
        if (!current) {
            return std::unexpected(current.error());
        }
        for (const auto& slot : current->slots()) {
            if (slot.occupied()) {
                // Lê o registro da página já carregada, sem novo acesso ao arquivo.
                auto bytes = current->read(slot.id);
                if (!bytes) {
                    return std::unexpected(bytes.error());
                }
                records.push_back(HeapRecord{RecordId{current_id, slot.id, slot.generation},
                                             std::move(*bytes)});
            }
        }
        // Encerra ao encontrar o sentinela de fim da cadeia.
        const auto next = current->next_page();
        if (!next) {
            return records;
        }
        // Avança para a página ligada.
        current_id = *next;
    }
}

Result<HeapPageSlice> TableHeap::read_page_records(PageId id) {
    // Uma leitura de página de dados — a unidade que o critério TTFR conta.
    ++data_pages_read_;
    auto current = load_trusted(id);
    if (!current) {
        return std::unexpected(current.error());
    }
    HeapPageSlice slice;
    for (const auto& slot : current->slots()) {
        if (slot.occupied()) {
            auto bytes = current->read(slot.id);
            if (!bytes) {
                return std::unexpected(bytes.error());
            }
            slice.records.push_back(
                HeapRecord{RecordId{id, slot.id, slot.generation}, std::move(*bytes)});
        }
    }
    slice.next = current->next_page();
    return slice;
}

// Percorre e resume a cadeia completa.
Result<std::vector<TableHeapPageInfo>> TableHeap::layout() {
    // Detecta ciclos antes que o percurso possa ficar infinito.
    std::unordered_set<std::uint64_t> visited;
    // Acumula uma entrada para cada página.
    std::vector<TableHeapPageInfo> pages;
    // Só substitui o índice atual depois que toda a cadeia for validada.
    std::unordered_set<std::uint64_t> page_ids;
    std::map<std::uint64_t, std::size_t> insertion_capacity_by_page;
    // Um heap vazio não possui slotted pages depois da raiz dedicada.
    if (!first_) {
        if (last_ || page_count_ != 0 || record_count_ != 0) {
            return std::unexpected(
                Error{ErrorCode::corrupt_page, "empty TableHeap metadata is inconsistent"});
        }
        page_ids_.clear();
        insertion_capacity_by_page_.clear();
        return pages;
    }
    // Começa na primeira página de dados persistida na raiz.
    auto current_id = *first_;
    std::optional<PageId> expected_previous;
    std::uint64_t actual_record_count = 0;

    // Cada repetição visita exatamente uma página nova.
    while (true) {
        if (auto visit = visit_page(visited, current_id); !visit) {
            return std::unexpected(visit.error());
        }
        // Carrega e valida a página atual.
        auto current = load(current_id);
        if (!current) {
            return std::unexpected(current.error());
        }
        // Cada ligação reversa precisa corresponder ao percurso iniciado na raiz.
        const auto previous = current->previous_page();
        if (previous != expected_previous) {
            return std::unexpected(Error{
                ErrorCode::corrupt_page,
                "TableHeap page has an inconsistent previous-page link",
            });
        }
        // Lê a ligação uma única vez para resumo e avanço.
        const auto next = current->next_page();
        page_ids.insert(current_id.value);
        insertion_capacity_by_page[current_id.value] = current->insertion_capacity();
        actual_record_count += current->record_count();
        // Guarda apenas metadados compreensíveis.
        pages.push_back(TableHeapPageInfo{
            current_id,
            previous,
            next,
            current->record_count(),
            current->free_space(),
        });
        // Encerra ao encontrar o sentinela de fim da cadeia.
        if (!next) {
            if (!last_ || current_id != *last_ || pages.size() != page_count_ ||
                actual_record_count != record_count_) {
                return std::unexpected(Error{
                    ErrorCode::corrupt_page,
                    "TableHeap root counters or chain endpoints are inconsistent",
                });
            }
            page_ids_ = std::move(page_ids);
            insertion_capacity_by_page_ = std::move(insertion_capacity_by_page);
            return pages;
        }
        // Avança para a página ligada.
        expected_previous = current_id;
        current_id = *next;
    }
}

// Substitui um registro, movendo-o para outra página somente quando necessário.
Result<RecordId> TableHeap::update(RecordId id, std::span<const std::byte> record) {
    // A página precisa pertencer ao heap, como validam read() e erase().
    if (!page_ids_.contains(id.page.value)) {
        return std::unexpected(Error{
            ErrorCode::record_not_found,
            "record page does not belong to this TableHeap",
        });
    }
    // Carrega a página uma única vez; erase() usa exatamente este padrão.
    auto page = load_trusted(id.page);
    if (!page) {
        return std::unexpected(page.error());
    }
    // Um SlotId reutilizado não pode satisfazer um RecordId antigo.
    auto generation = page->generation(id.slot);
    if (!generation || *generation != id.generation) {
        return std::unexpected(Error{
            ErrorCode::record_not_found,
            "record generation does not match the occupied slot",
        });
    }
    // Tenta preservar página, slot e geração.
    auto updated = page->update(id.slot, record);
    if (updated) {
        if (auto written = file_->write(id.page, page->page()); !written) {
            return std::unexpected(written.error());
        }
        insertion_capacity_by_page_[id.page.value] = page->insertion_capacity();
        return id;
    }
    // Erros diferentes de falta de espaço não permitem realocação.
    if (updated.error().code != ErrorCode::page_full) {
        return std::unexpected(updated.error());
    }
    // Insere primeiro a nova versão para não perder a antiga em caso de falha.
    auto replacement = insert(record);
    if (!replacement) {
        return std::unexpected(replacement.error());
    }
    // Remove o endereço antigo depois que a substituição está persistível.
    if (auto removed = erase(id); !removed) {
        return std::unexpected(removed.error());
    }
    // Uma realocação produz um novo RecordId.
    return *replacement;
}

// Remove um registro por acesso direto e mantém as duas direções da cadeia.
Result<void> TableHeap::erase(RecordId id) {
    if (!page_ids_.contains(id.page.value)) {
        return std::unexpected(Error{
            ErrorCode::record_not_found,
            "record page does not belong to this TableHeap",
        });
    }
    auto current = load_trusted(id.page);
    if (!current) {
        return std::unexpected(current.error());
    }
    // Compara a geração antes de permitir que um slot seja removido.
    auto generation = current->generation(id.slot);
    if (!generation || *generation != id.generation) {
        return std::unexpected(Error{
            ErrorCode::record_not_found,
            "record generation does not match the occupied slot",
        });
    }
    // Recupera a capacidade e compacta a página.
    if (auto removed = current->erase(id.slot); !removed) {
        return std::unexpected(removed.error());
    }

    if (record_count_ == 0 || page_count_ == 0) {
        return std::unexpected(
            Error{ErrorCode::corrupt_page, "TableHeap counters underflow during erase"});
    }

    // Páginas ainda ocupadas continuam na cadeia.
    if (current->record_count() != 0) {
        if (auto written = file_->write(id.page, current->page()); !written) {
            return std::unexpected(written.error());
        }
        --record_count_;
        insertion_capacity_by_page_[id.page.value] = current->insertion_capacity();
        if (auto persisted = persist_root(); !persisted) {
            return std::unexpected(persisted.error());
        }
        return {};
    }

    // Uma página vazia é retirada, inclusive quando está em um dos extremos.
    const auto previous_id = current->previous_page();
    const auto next_id = current->next_page();
    if ((!previous_id && first_ != std::optional<PageId>{id.page}) ||
        (!next_id && last_ != std::optional<PageId>{id.page})) {
        return std::unexpected(Error{
            ErrorCode::corrupt_page,
            "TableHeap data-chain endpoint does not match root metadata",
        });
    }

    if (previous_id) {
        auto previous = load_trusted(*previous_id);
        if (!previous) {
            return std::unexpected(previous.error());
        }
        if (previous->next_page() != std::optional<PageId>{id.page}) {
            return std::unexpected(Error{
                ErrorCode::corrupt_page,
                "TableHeap previous page does not link to the removed page",
            });
        }
        if (auto linked = previous->set_next_page(next_id); !linked) {
            return std::unexpected(linked.error());
        }
        if (auto written = file_->write(*previous_id, previous->page()); !written) {
            return std::unexpected(written.error());
        }
    }
    if (next_id) {
        auto next = load_trusted(*next_id);
        if (!next) {
            return std::unexpected(next.error());
        }
        if (next->previous_page() != std::optional<PageId>{id.page}) {
            return std::unexpected(Error{
                ErrorCode::corrupt_page,
                "TableHeap next page does not link back to the removed page",
            });
        }
        if (auto linked = next->set_previous_page(previous_id); !linked) {
            return std::unexpected(linked.error());
        }
        if (auto written = file_->write(*next_id, next->page()); !written) {
            return std::unexpected(written.error());
        }
    }

    if (!previous_id) {
        first_ = next_id;
    }
    if (!next_id) {
        last_ = previous_id;
    }
    --page_count_;
    --record_count_;
    page_ids_.erase(id.page.value);
    insertion_capacity_by_page_.erase(id.page.value);
    if (page_count_ == 0) {
        first_.reset();
        last_.reset();
    }
    if (auto persisted = persist_root(); !persisted) {
        return std::unexpected(persisted.error());
    }
    // A página física órfã será reaproveitada por um futuro free-page manager.
    return {};
}

} // namespace modb::storage
