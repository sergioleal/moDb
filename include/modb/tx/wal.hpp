#pragma once

// Importa Result e os códigos de erro do WAL.
#include "modb/error.hpp"
// Importa NativeFile, dono do descritor do arquivo de log.
#include "modb/storage/native_file.hpp"
// Importa PageId e page_size (o log guarda imagens de página inteiras).
#include "modb/storage/page.hpp"

// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza caminhos do arquivo de log.
#include <filesystem>
// Disponibiliza a fábrica injetável do arquivo do WAL.
#include <functional>
// Disponibiliza a posse do sink por ponteiro.
#include <memory>
// Disponibiliza a visão sem cópia da imagem de página.
#include <span>
// Disponibiliza o buffer dos registros lidos.
#include <vector>

namespace modb::tx {

// Abstração mínima do arquivo por trás do WAL: escrita posicional + sync. Existe
// para os testes injetarem falhas de I/O reais (failpoints) sem que o WAL
// conheça o NativeFile diretamente. A implementação de produção encaminha para
// storage::NativeFile.
class WalSink {
public:
    virtual ~WalSink() = default;
    [[nodiscard]] virtual Result<void> write_at(std::uint64_t offset,
                                                std::span<const std::byte> source) = 0;
    [[nodiscard]] virtual Result<void> sync() = 0;
};

// Fábrica de sinks: recria o arquivo do WAL no caminho dado, pronto para o
// cabeçalho. Testes trocam a fábrica para embrulhar o sink num FailpointFile.
using WalFileFactory =
    std::function<Result<std::unique_ptr<WalSink>>(const std::filesystem::path&)>;

// Fábrica de produção: remove um WAL residual e abre um NativeFile novo.
[[nodiscard]] Result<std::unique_ptr<WalSink>> open_native_wal_sink(
    const std::filesystem::path& path);

// Tipos de registro do WAL. Os valores fazem parte do formato: nunca renumerar.
enum class WalRecordType : std::uint8_t {
    begin = 1,
    page_image = 2,
    commit = 3,
    checkpoint = 4,
};

// Um registro decodificado do WAL.
struct WalRecord {
    std::uint64_t lsn{};
    std::uint64_t tx_id{};
    WalRecordType type{WalRecordType::begin};
    std::uint64_t page_id{};
    std::vector<std::byte> payload; // vazio, exceto para page_image (página inteira)

    friend bool operator==(const WalRecord&, const WalRecord&) = default;
};

// Bytes do cabeçalho do arquivo WAL:
//   | "MOWL" 4 | versão u16 | reservado u16 | page_size u32 | reservado u32*3 |
inline constexpr std::size_t wal_header_size = 32;
inline constexpr std::uint16_t wal_version = 1;

// Write-ahead log redo-only: registros anexados sequencialmente, cada um com um
// CRC32 que cobre do `lsn` ao fim do payload. Um registro com CRC inválido ou
// truncado marca o fim lógico do log (tudo depois é descartado na leitura).
//
// O arquivo é gerenciado por si só: `create` recria do zero e a recuperação o
// remove após aplicar — sem necessidade de truncate no NativeFile.
class Wal {
public:
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;
    Wal(Wal&&) noexcept = default;
    Wal& operator=(Wal&&) noexcept = default;
    ~Wal() = default;

    // Cria (ou recria) o arquivo de log com o cabeçalho, pronto para anexar.
    [[nodiscard]] static Result<Wal> create(const std::filesystem::path& path);
    // Igual, mas usando uma fábrica de sink específica (injeção de failpoint).
    [[nodiscard]] static Result<Wal> create(const std::filesystem::path& path,
                                            const WalFileFactory& factory);

    // Anexa os registros abaixo, atribuindo LSNs monotônicos internamente.
    [[nodiscard]] Result<void> append_begin(std::uint64_t tx_id);
    [[nodiscard]] Result<void> append_page_image(std::uint64_t tx_id, storage::PageId page_id,
                                                 std::span<const std::byte> page);
    [[nodiscard]] Result<void> append_commit(std::uint64_t tx_id);

    // Força a durabilidade dos registros já anexados (sync do dispositivo).
    [[nodiscard]] Result<void> sync();

    // Lê todos os registros íntegros de um arquivo WAL até o fim lógico.
    [[nodiscard]] static Result<std::vector<WalRecord>> read_all(
        const std::filesystem::path& path);

private:
    Wal(std::unique_ptr<WalSink> sink, std::uint64_t write_offset) noexcept
        : sink_{std::move(sink)}, write_offset_{write_offset} {}

    // Serializa e anexa um registro completo (com CRC).
    [[nodiscard]] Result<void> append(WalRecordType type, std::uint64_t tx_id,
                                      std::uint64_t page_id, std::span<const std::byte> payload);

    std::unique_ptr<WalSink> sink_;
    std::uint64_t write_offset_{wal_header_size};
    std::uint64_t next_lsn_{1};
};

} // namespace modb::tx
