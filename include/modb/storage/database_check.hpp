#pragma once

// Importa Result e Error para o relatório de integridade.
#include "modb/error.hpp"
// Importa Page e PageId usados na classificação.
#include "modb/storage/page.hpp"
// Importa SlotId usado ao apontar registros corrompidos no nível L4.
#include "modb/storage/slotted_page.hpp"

// Disponibiliza caminhos do arquivo verificado.
#include <filesystem>
// Disponibiliza inteiros com largura fixa.
#include <cstdint>
// Disponibiliza erro opcional por página.
#include <optional>
// Disponibiliza as listas do relatório.
#include <vector>

namespace modb::storage {

// Identifica o tipo inferido pelos primeiros bytes de uma página.
enum class PageKind {
    // Página zero com metadados do arquivo.
    superblock,
    // Página alocada e ainda zerada.
    unformatted,
    // Página de registros com assinatura SLPG.
    slotted,
    // Raiz dedicada de TableHeap com assinatura THRP.
    table_heap_root,
    // Página raiz do banco OO com assinatura DBRT (ADR-004).
    database_root,
    // Página de diretório do mapa de identidade com assinatura IDMD (ADR-005).
    identity_directory,
    // Página de entradas do mapa de identidade com assinatura IDMP (ADR-005).
    identity_entries,
    // Assinatura não reconhecida.
    unknown,
};

// Resume o resultado da validação de uma página de dados.
struct PageCheckResult {
    // Identifica a página inspecionada.
    PageId id;
    // Informa o tipo classificado pelos bytes iniciais.
    PageKind kind;
    // Guarda a falha de L2 quando a página é inválida.
    std::optional<Error> error;

    // Permite comparar resultados em testes.
    friend bool operator==(const PageCheckResult&, const PageCheckResult&) = default;
};

// Aponta um registro que não pôde ser decodificado no nível L4.
struct RecordCheckError {
    // Página que contém o registro inválido.
    PageId page;
    // Slot do registro dentro da página.
    SlotId slot;
    // Erro devolvido pela decodificação.
    Error error;

    // Permite comparar resultados em testes.
    friend bool operator==(const RecordCheckError&, const RecordCheckError&) = default;
};

// Agrupa o inventário e as falhas das camadas L2, L3 e L4.
struct DatabaseCheckReport {
    // Quantidade total de páginas, incluindo o superbloco.
    std::uint64_t page_count{};
    // Uma entrada para cada página de dados (PageId >= 1).
    std::vector<PageCheckResult> pages;
    // Falhas de abertura/cadeia das raízes TableHeap, incluindo cadeias compartilhadas.
    std::vector<Error> heap_errors;
    // Registros que falharam ao ser decodificados (nível L4).
    std::vector<RecordCheckError> record_errors;

    // Indica ausência de erros nas camadas L2, L3 e L4.
    [[nodiscard]] bool ok() const noexcept {
        if (!heap_errors.empty() || !record_errors.empty()) {
            return false;
        }
        for (const auto& page : pages) {
            if (page.error) {
                return false;
            }
        }
        return true;
    }
};

// Classifica uma página pelos magic bytes ou pelo padrão zerado.
[[nodiscard]] PageKind classify_page(const Page& page) noexcept;

// Executa L1–L3: abre o arquivo, inventaria páginas e valida heaps THRP.
[[nodiscard]] Result<DatabaseCheckReport> check_database(const std::filesystem::path& path);

} // namespace modb::storage
