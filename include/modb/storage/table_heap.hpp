#pragma once

// Importa Result para todas as operações persistentes.
#include "modb/error.hpp"
// Importa o arquivo que possui as páginas.
#include "modb/storage/page_file.hpp"
#include "modb/storage/scratch_page_pool.hpp"
// Importa RecordId e SlottedPage.
#include "modb/storage/slotted_page.hpp"

// Disponibiliza std::byte e std::size_t.
#include <cstddef>
// Disponibiliza inteiros com largura fixa.
#include <cstdint>
// Disponibiliza ownership movível do pool, que contém mutexes não movíveis.
#include <memory>
// Disponibiliza uma próxima página opcional.
#include <optional>
// Disponibiliza visões dos registros recebidos.
#include <span>
// Disponibiliza o mapa ordenado por PageId do índice de capacidade.
#include <map>
// Disponibiliza os vetores retornados por leitura, scan e layout.
#include <unordered_set>
#include <vector>

namespace modb::storage {

// Valida assinatura, versão e metadados básicos de uma página THRP.
[[nodiscard]] Result<void> validate_table_heap_root(const Page& page);

// Resultado de um reparo de raiz de TableHeap.
struct TableHeapRepairReport {
    // Páginas de dados encontradas percorrendo a cadeia.
    std::uint64_t page_count;
    // Registros vivos somados ao longo da cadeia.
    std::uint64_t record_count;
    // Indica se a raiz precisou ser reescrita para corrigir divergências.
    bool root_rewritten;

    // Permite comparar relatórios em testes.
    friend bool operator==(const TableHeapRepairReport&, const TableHeapRepairReport&) = default;
};

// Reconstrói extremos e contadores da raiz a partir da cadeia autodescritiva,
// tornando abrível um heap cujos metadados divergiram após uma falha parcial.
// Um ciclo ou uma página de dados corrompida impedem o reparo e retornam erro.
[[nodiscard]] Result<TableHeapRepairReport> repair_table_heap(PageFile& file, PageId root);

// Resume o estado de uma página pertencente a um TableHeap.
struct TableHeapPageInfo {
    // Identifica a página no arquivo.
    PageId id;
    // Indica a página anterior ou o início da cadeia.
    std::optional<PageId> previous;
    // Indica a próxima página ou o fim da cadeia.
    std::optional<PageId> next;
    // Informa quantos slots estão ocupados.
    std::uint16_t record_count;
    // Informa quantos bytes ainda estão livres.
    std::size_t free_space;

    // Permite comparar layouts em testes.
    friend bool operator==(const TableHeapPageInfo&, const TableHeapPageInfo&) = default;
};

// Associa um endereço estável ao conteúdo bruto do registro apontado por ele.
struct HeapRecord {
    // Endereço persistido do registro.
    RecordId id;
    // Cópia dos bytes do registro, independente da vida da página.
    std::vector<std::byte> bytes;

    // Permite comparar resultados de scan em testes.
    friend bool operator==(const HeapRecord&, const HeapRecord&) = default;
};

// Registros de uma única página de dados e o ponteiro para a próxima da cadeia.
// É o átomo de um scan preguiçoso (Fase 7A): o consumidor lê uma página, produz
// seus registros e só então segue para `next` — nunca materializa o heap todo.
struct HeapPageSlice {
    std::vector<HeapRecord> records;
    std::optional<PageId> next;
};

// Trata uma cadeia de slotted pages como uma coleção única de registros.
class TableHeap {
public:
    // Aloca e formata a raiz dedicada de um heap vazio.
    [[nodiscard]] static Result<TableHeap> create(PageFile& file,
                                                   std::size_t scratch_page_count = 1);
    // Abre e valida toda a cadeia iniciada pela página raiz.
    [[nodiscard]] static Result<TableHeap> open(PageFile& file, PageId root,
                                                 std::size_t scratch_page_count = 1);

    // Insere automaticamente na primeira página com espaço ou cria outra.
    [[nodiscard]] Result<RecordId> insert(std::span<const std::byte> record);
    // Lê um registro somente se sua página pertencer à cadeia.
    [[nodiscard]] Result<std::vector<std::byte>> read(RecordId id);
    // Retorna todos os RecordIds na ordem das páginas e slots.
    [[nodiscard]] Result<std::vector<RecordId>> scan();
    // Retorna endereços e bytes de todos os registros lendo cada página uma vez.
    [[nodiscard]] Result<std::vector<HeapRecord>> scan_records();
    // Lê UMA página de dados e devolve seus registros vivos e a próxima da
    // cadeia (Fase 7A): o átomo de um scan preguiçoso. Incrementa o contador de
    // páginas de dados lidas.
    [[nodiscard]] Result<HeapPageSlice> read_page_records(PageId id);
    // Contador de páginas de dados lidas por `read_page_records` — instrumenta o
    // critério TTFR da Fase 7A (`limit 1` deve ler ≤ 2 páginas). Diagnóstico de
    // sessão, zerável.
    [[nodiscard]] std::uint64_t data_pages_read() const noexcept { return data_pages_read_; }
    void reset_data_pages_read() noexcept { data_pages_read_ = 0; }
    // Retorna informações compreensíveis de todas as páginas.
    [[nodiscard]] Result<std::vector<TableHeapPageInfo>> layout();
    // Retorna a página usada para abrir o heap.
    [[nodiscard]] PageId root_page() const noexcept { return root_; }
    // Retorna os extremos persistidos da cadeia de dados.
    [[nodiscard]] std::optional<PageId> first_page() const noexcept { return first_; }
    [[nodiscard]] std::optional<PageId> last_page() const noexcept { return last_; }
    // Retorna os contadores persistidos na raiz dedicada.
    [[nodiscard]] std::uint64_t page_count() const noexcept { return page_count_; }
    [[nodiscard]] std::uint64_t record_count() const noexcept { return record_count_; }

    // Substitui um registro e informa seu endereço atual.
    [[nodiscard]] Result<RecordId> update(RecordId id, std::span<const std::byte> record);
    // Remove um registro e desconecta sua página quando ela fica vazia.
    [[nodiscard]] Result<void> erase(RecordId id);

private:
    // Somente create e open podem associar arquivo e raiz. O pool já vem pronto
    // da fábrica ScratchPagePool::create, então o construtor não pode falhar.
    TableHeap(PageFile& file, PageId root, std::unique_ptr<ScratchPagePool> scratch_page_pool,
              std::optional<PageId> first = std::nullopt,
              std::optional<PageId> last = std::nullopt,
              std::uint64_t page_count = 0, std::uint64_t record_count = 0)
        : file_{&file},
          root_{root},
          scratch_page_pool_{std::move(scratch_page_pool)},
          first_{first},
          last_{last},
          page_count_{page_count},
          record_count_{record_count} {}

    // Lê e valida uma página da cadeia (usado ao abrir e validar tudo).
    [[nodiscard]] Result<SlottedPage> load(PageId id);
    // Lê sem revalidar uma página já confiável nesta sessão (caminhos quentes).
    [[nodiscard]] Result<SlottedPage> load_trusted(PageId id);
    // Persiste os ponteiros e contadores da raiz dedicada.
    [[nodiscard]] Result<void> persist_root();

    // Aponta para o arquivo cuja vida é controlada pelo chamador.
    PageFile* file_;
    // Guarda a identidade estável da raiz dedicada.
    PageId root_;
    // Possui buffers fixos reutilizados pelas leituras do heap.
    std::unique_ptr<ScratchPagePool> scratch_page_pool_;
    // Identificam os extremos da cadeia de páginas de dados.
    std::optional<PageId> first_;
    std::optional<PageId> last_;
    // Espelham os contadores persistidos na raiz dedicada.
    std::uint64_t page_count_{};
    std::uint64_t record_count_{};
    // Conta páginas de dados lidas por `read_page_records` (instrumentação 7A).
    std::uint64_t data_pages_read_{};
    // Permite validar e localizar diretamente páginas pertencentes ao heap.
    std::unordered_set<std::uint64_t> page_ids_;
    // Evita leituras de páginas que certamente não comportam uma inserção.
    // std::map mantém a ordem por PageId (idade de alocação), tornando a escolha
    // de página determinística entre execuções e plataformas.
    std::map<std::uint64_t, std::size_t> insertion_capacity_by_page_;
};

} // namespace modb::storage
