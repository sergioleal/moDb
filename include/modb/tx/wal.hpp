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

// Fábrica de produção: remove um WAL residual e abre um NativeFile novo
// (modo efêmero / failpoints). Preferir `open_append_wal_sink` no caminho durável.
[[nodiscard]] Result<std::unique_ptr<WalSink>> open_native_wal_sink(
    const std::filesystem::path& path);

// Abre (ou cria) o WAL para append sem apagar o conteúdo existente.
[[nodiscard]] Result<std::unique_ptr<WalSink>> open_append_wal_sink(
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
    std::vector<std::byte> payload; // page_image: página; commit v2: commit_lsn u64

    // Para registros commit v2, extrai o commit_lsn do payload (0 se ausente).
    [[nodiscard]] std::uint64_t commit_lsn() const noexcept;

    friend bool operator==(const WalRecord&, const WalRecord&) = default;
};

// Bytes do cabeçalho do arquivo WAL:
//   | "MOWL" 4 | versão u16 | reservado u16 | page_size u32 | reservado u32*3 |
inline constexpr std::size_t wal_header_size = 32;
inline constexpr std::uint16_t wal_version = 2;
inline constexpr std::uint16_t wal_version_v1 = 1;

// Write-ahead log redo-only (v2 durável): LSN global injetado na abertura,
// registros anexados, commit carrega `commit_lsn` no payload. O arquivo não é
// apagado a cada commit — checkpoint é posição (DBRT), não remoção.
class Wal {
public:
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;
    Wal(Wal&&) noexcept = default;
    Wal& operator=(Wal&&) noexcept = default;
    ~Wal() = default;

    // Cria (ou recria) o arquivo de log com o cabeçalho (modo efêmero/failpoint).
    [[nodiscard]] static Result<Wal> create(const std::filesystem::path& path);
    [[nodiscard]] static Result<Wal> create(const std::filesystem::path& path,
                                            const WalFileFactory& factory);
    // Abre para append (cria se ausente). `starting_lsn` é o próximo LSN global.
    [[nodiscard]] static Result<Wal> open_durable(const std::filesystem::path& path,
                                                   std::uint64_t starting_lsn);
    [[nodiscard]] static Result<Wal> open_durable(const std::filesystem::path& path,
                                                   std::uint64_t starting_lsn,
                                                   const WalFileFactory& append_factory);

    [[nodiscard]] std::uint64_t next_lsn() const noexcept { return next_lsn_; }
    [[nodiscard]] std::uint64_t last_appended_lsn() const noexcept {
        return next_lsn_ == 0 ? 0 : next_lsn_ - 1;
    }

    // Anexa os registros abaixo, atribuindo LSNs monotônicos a partir de next_lsn_.
    [[nodiscard]] Result<void> append_begin(std::uint64_t tx_id);
    [[nodiscard]] Result<void> append_page_image(std::uint64_t tx_id, storage::PageId page_id,
                                                 std::span<const std::byte> page);
    // Commit v2: payload = commit_lsn (LSN deste registro) em little-endian.
    [[nodiscard]] Result<void> append_commit(std::uint64_t tx_id);

    // Força a durabilidade dos registros já anexados (sync do dispositivo).
    [[nodiscard]] Result<void> sync();

    // Lê todos os registros íntegros até o fim lógico (truncamento/CRC = fim).
    [[nodiscard]] static Result<std::vector<WalRecord>> read_all(
        const std::filesystem::path& path);

    // Lê registros com lsn >= from_lsn (fim lógico suave).
    [[nodiscard]] static Result<std::vector<WalRecord>> read_from(
        const std::filesystem::path& path, std::uint64_t from_lsn);

    // Leitura para replicação: truncamento ou CRC inválido no meio = erro
    // (não fim lógico silencioso).
    [[nodiscard]] static Result<std::vector<WalRecord>> read_for_replication(
        const std::filesystem::path& path, std::uint64_t from_lsn);

private:
    Wal(std::unique_ptr<WalSink> sink, std::uint64_t write_offset,
        std::uint64_t next_lsn) noexcept
        : sink_{std::move(sink)}, write_offset_{write_offset}, next_lsn_{next_lsn} {}

    [[nodiscard]] Result<void> append(WalRecordType type, std::uint64_t tx_id,
                                      std::uint64_t page_id, std::span<const std::byte> payload);

    std::unique_ptr<WalSink> sink_;
    std::uint64_t write_offset_{wal_header_size};
    std::uint64_t next_lsn_{1};
};

} // namespace modb::tx
