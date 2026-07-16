#pragma once

// Importa Result para retornar erros de página e slot.
#include "modb/error.hpp"
// Importa Page e PageId.
#include "modb/storage/page.hpp"

// Disponibiliza std::byte e std::size_t.
#include <cstddef>
// Disponibiliza inteiros com largura fixa.
#include <cstdint>
// Disponibiliza um PageId que pode não existir no fim da cadeia.
#include <optional>
// Disponibiliza uma visão dos bytes recebidos na inserção.
#include <span>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza o vetor que possui uma cópia do registro lido.
#include <vector>

namespace modb::storage {

// Define a versão persistida do layout de slotted page.
inline constexpr std::uint8_t slotted_page_format_version = 3;
// Define quantos bytes pertencem ao cabeçalho fixo.
inline constexpr std::size_t slotted_page_header_size = 28;
// Define quantos bytes pertencem a cada entrada do diretório.
inline constexpr std::size_t slotted_page_slot_size = 8;
// Garante que a página comporta o cabeçalho, um slot e ao menos um byte de
// registro. Sem esta verificação, um MODB_PAGE_SIZE entre 29 e 36 faria a
// subtração abaixo dar wrap em std::size_t e transformar o limite de registro em
// um valor gigante, anulando todos os guardas de record_too_large.
static_assert(page_size >= slotted_page_header_size + slotted_page_slot_size + 1,
              "MODB_PAGE_SIZE must leave room for the header, one slot and one record byte");

// Define o maior registro que cabe junto com seu primeiro slot.
inline constexpr std::size_t slotted_page_max_record_size =
    page_size - slotted_page_header_size - slotted_page_slot_size;

// Identifica uma entrada no diretório interno de uma slotted page.
struct SlotId {
    // Guarda o índice do slot dentro da página.
    std::uint16_t value{};

    // Permite comparar dois identificadores diretamente.
    friend bool operator==(SlotId, SlotId) = default;
};

// Identifica um registro combinando a página e seu slot interno.
struct RecordId {
    // Identifica a página armazenada no PageFile.
    PageId page;
    // Identifica o registro dentro dessa página.
    SlotId slot;
    // Distingue reutilizações sucessivas do mesmo SlotId.
    std::uint16_t generation{};

    // Permite comparar endereços completos de registros.
    friend bool operator==(RecordId, RecordId) = default;
};

// Descreve onde um slot e seu registro estão localizados fisicamente.
struct SlotInfo {
    // Identifica a entrada no diretório.
    SlotId id;
    // Indica o primeiro byte do registro.
    std::uint16_t record_offset;
    // Indica quantos bytes pertencem ao registro.
    std::uint16_t record_size;
    // Indica o espaço reservado, incluindo uma possível margem não utilizada.
    std::uint16_t record_capacity;
    // Identifica a ocupação atual do slot.
    std::uint16_t generation;

    // Informa se o slot aponta para um registro vivo.
    [[nodiscard]] bool occupied() const noexcept { return record_size != 0; }

    // Permite comparar informações de slots em testes.
    friend bool operator==(const SlotInfo&, const SlotInfo&) = default;
};

// Organiza registros de tamanho variável dentro de uma página fixa.
class SlottedPage {
public:
    // Cria uma página de registros vazia e formatada.
    [[nodiscard]] static SlottedPage create();
    // Valida e reconstrói uma slotted page a partir de bytes existentes.
    [[nodiscard]] static Result<SlottedPage> from_page(Page page);
    // Reconstrói sem revalidar: use apenas em páginas já validadas nesta sessão
    // (o TableHeap valida toda a cadeia ao abrir e mantém as invariantes depois).
    [[nodiscard]] static SlottedPage from_trusted_page(Page page);

    // Insere uma cópia do registro e retorna seu slot estável.
    [[nodiscard]] Result<SlotId> insert(std::span<const std::byte> record);
    // Lê e copia os bytes pertencentes ao slot solicitado.
    [[nodiscard]] Result<std::vector<std::byte>> read(SlotId slot) const;
    // Retorna a geração atual de um slot ocupado.
    [[nodiscard]] Result<std::uint16_t> generation(SlotId slot) const;
    // Substitui o conteúdo, preservando SlotId e geração.
    [[nodiscard]] Result<void> update(SlotId slot, std::span<const std::byte> record);
    // Remove o conteúdo e disponibiliza o slot para reutilização.
    [[nodiscard]] Result<void> erase(SlotId slot);

    // Retorna a quantidade de registros atualmente armazenados.
    [[nodiscard]] std::uint16_t record_count() const noexcept;
    // Retorna a quantidade total de entradas, incluindo slots livres.
    [[nodiscard]] std::uint16_t slot_count() const noexcept;
    // Retorna os bytes livres entre diretório e registros.
    [[nodiscard]] std::size_t free_space() const noexcept;
    // Retorna o maior registro inserível considerando possível entrada de slot.
    [[nodiscard]] std::size_t insertion_capacity() const noexcept;
    // Retorna onde termina o cabeçalho e o diretório de slots.
    [[nodiscard]] std::uint16_t free_start() const noexcept;
    // Retorna onde começa a área física de registros.
    [[nodiscard]] std::uint16_t free_end() const noexcept;
    // Retorna uma descrição somente leitura de cada entrada do diretório.
    [[nodiscard]] std::vector<SlotInfo> slots() const;
    // Retorna a próxima página da cadeia ou vazio quando esta é a última.
    [[nodiscard]] std::optional<PageId> next_page() const noexcept;
    // Atualiza a ligação, rejeitando a página zero reservada.
    [[nodiscard]] Result<void> set_next_page(std::optional<PageId> next);
    // Retorna a página anterior da cadeia ou vazio quando esta é a raiz.
    [[nodiscard]] std::optional<PageId> previous_page() const noexcept;
    // Atualiza a ligação reversa, rejeitando a página zero reservada.
    [[nodiscard]] Result<void> set_previous_page(std::optional<PageId> previous);
    // Expõe a página somente para persistência e sem permitir alteração externa.
    [[nodiscard]] const Page& page() const noexcept { return page_; }
    // Transfere a página completa para o chamador.
    [[nodiscard]] Page into_page() && noexcept { return std::move(page_); }

private:
    // Somente create e from_page constroem uma instância.
    explicit SlottedPage(Page page) : page_{std::move(page)} {}

    // Reagrupa todas as capacidades ocupadas no final da página.
    void compact();

    // Possui cabeçalho, diretório de slots e registros.
    Page page_;
};

} // namespace modb::storage
