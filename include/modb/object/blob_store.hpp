#pragma once

// Importa Result e os códigos de erro de blob.
#include "modb/error.hpp"
// Importa BlobId (identidade = PageId da primeira página, ADR-001).
#include "modb/object/ids.hpp"
// Importa PageFile, dono das páginas encadeadas do blob.
#include "modb/storage/page_file.hpp"

// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza o callback de leitura em streaming.
#include <functional>
// Disponibiliza a visão sem cópia dos dados de entrada.
#include <span>
// Disponibiliza o buffer devolvido pela leitura.
#include <vector>

namespace modb::object {

// Bytes fixos do cabeçalho de cada página de blob (assinatura BLBP):
//   | "BLBP" 4 | versão u16 | reservado u16 | next u64 | payload_length u32 | reservado u32 |
// seguidos por até page_size-24 bytes de dados.
inline constexpr std::size_t blob_header_size = 24;
// Versão do formato de página de blob.
inline constexpr std::uint16_t blob_page_version = 1;
// Dados úteis que cabem numa única página de blob.
inline constexpr std::size_t blob_page_capacity = storage::page_size - blob_header_size;

// Armazena binários maiores que uma página em uma cadeia de páginas BLBP. O
// objeto pai guarda apenas o BlobId (a primeira página); PersistentVector e
// afins constroem seus conteúdos por cima daqui (doc §12).
//
// O caller possui o PageFile e deve mantê-lo vivo enquanto o BlobStore existir.
class BlobStore {
public:
    // Liga a store ao arquivo; não aloca nada até a primeira gravação.
    explicit BlobStore(storage::PageFile& file) noexcept : file_{&file} {}

    // Informa se há uma transação ativa no arquivo de respaldo. As coleções
    // usam isto para exigir uma transação antes de qualquer escrita (Fase 5).
    [[nodiscard]] bool in_transaction() const noexcept { return file_->in_transaction(); }

    // Grava os bytes fatiados em páginas encadeadas e devolve o BlobId (a
    // primeira página). Um blob vazio ainda ocupa uma página com length 0.
    [[nodiscard]] Result<BlobId> create(std::span<const std::byte> data);

    // Lê o blob inteiro, validando magic/versão/comprimento e detectando ciclos.
    [[nodiscard]] Result<std::vector<std::byte>> read(BlobId id) const;

    // Percorre o blob em páginas, entregando cada fatia sem materializar tudo.
    [[nodiscard]] Result<void> read_chunks(
        BlobId id, const std::function<Result<void>(std::span<const std::byte>)>& visitor) const;

    // Reescreve o conteúdo mantendo a primeira página estável (o BlobId não
    // muda). Reaproveita as páginas da cadeia antiga, estende ou apara conforme
    // o novo tamanho. Se id for nulo, comporta-se como create.
    [[nodiscard]] Result<BlobId> rewrite(BlobId id, std::span<const std::byte> data);

    // Zera as páginas da cadeia (marca como não usadas). No MVP não há free
    // list: as páginas ficam órfãs no arquivo, disponíveis para o database_check.
    [[nodiscard]] Result<void> remove(BlobId id);

private:
    // Arquivo cuja vida é controlada pelo chamador.
    storage::PageFile* file_;
};

} // namespace modb::object
