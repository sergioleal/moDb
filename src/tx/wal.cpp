// Importa a interface do WAL.
#include "modb/tx/wal.hpp"

// Importa store_le/load_le (a implementação única de little-endian).
#include "modb/storage/endian.hpp"
// Importa NativeFile, base do sink de produção.
#include "modb/storage/native_file.hpp"

// Disponibiliza std::copy/std::equal.
#include <algorithm>
// Disponibiliza std::make_unique.
#include <memory>
// Disponibiliza os blocos fixos de assinatura e cabeçalho.
#include <array>
// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza std::error_code na remoção/consulta do arquivo.
#include <system_error>
// Disponibiliza std::move.
#include <utility>

namespace modb::tx {

// Reaproveita a implementação única de little-endian da camada de storage.
using storage::load_le;
using storage::store_le;

namespace {

// Assinatura do arquivo WAL.
constexpr std::array<std::byte, 4> wal_magic{
    std::byte{'M'}, std::byte{'O'}, std::byte{'W'}, std::byte{'L'}};

// Parte fixa de um registro antes do payload: lsn(8)+tx(8)+tipo(1)+page(8)+len(4).
constexpr std::size_t record_fixed_size = 8 + 8 + 1 + 8 + 4;
// Bytes do CRC ao fim de cada registro.
constexpr std::size_t record_crc_size = 4;

// CRC32 (IEEE 802.3, polinômio refletido 0xEDB88320) — tabela calculada uma vez.
std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
            }
            result[index] = value;
        }
        return result;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        const auto octet = std::to_integer<std::uint8_t>(byte);
        crc = table[(crc ^ octet) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

// Monta o cabeçalho de 32 bytes do arquivo WAL.
std::array<std::byte, wal_header_size> make_header() {
    std::array<std::byte, wal_header_size> header{};
    std::copy(wal_magic.begin(), wal_magic.end(), header.begin());
    store_le(std::span<std::byte>{header}.subspan(4, 2), wal_version);
    store_le(std::span<std::byte>{header}.subspan(8, 4),
             static_cast<std::uint32_t>(storage::page_size));
    return header;
}

Error io_error(std::string message) { return Error{ErrorCode::io_error, std::move(message)}; }

// Sink de produção: encaminha escrita/sync para um NativeFile.
class NativeWalSink final : public WalSink {
public:
    explicit NativeWalSink(storage::NativeFile file) noexcept : file_{std::move(file)} {}
    Result<void> write_at(std::uint64_t offset, std::span<const std::byte> source) override {
        return file_.write_at(offset, source);
    }
    Result<void> sync() override { return file_.sync(); }

private:
    storage::NativeFile file_;
};

} // namespace

Result<std::unique_ptr<WalSink>> open_native_wal_sink(const std::filesystem::path& path) {
    // Recomeça do zero: um WAL residual de uma sessão anterior é substituído.
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    auto file = storage::NativeFile::open(path, storage::NativeFile::Mode::create_new);
    if (!file) {
        return std::unexpected(file.error());
    }
    return std::make_unique<NativeWalSink>(std::move(*file));
}

Result<Wal> Wal::create(const std::filesystem::path& path) {
    return create(path, open_native_wal_sink);
}

Result<Wal> Wal::create(const std::filesystem::path& path, const WalFileFactory& factory) {
    auto sink = factory(path);
    if (!sink) {
        return std::unexpected(sink.error());
    }
    const auto header = make_header();
    if (auto written = (*sink)->write_at(0, header); !written) {
        return std::unexpected(written.error());
    }
    return Wal{std::move(*sink), wal_header_size};
}

Result<void> Wal::append(WalRecordType type, std::uint64_t tx_id, std::uint64_t page_id,
                         std::span<const std::byte> payload) {
    std::vector<std::byte> record(record_fixed_size + payload.size() + record_crc_size);
    const std::span<std::byte> view{record};
    store_le(view.subspan(0, 8), next_lsn_);
    store_le(view.subspan(8, 8), tx_id);
    view[16] = static_cast<std::byte>(static_cast<std::uint8_t>(type));
    store_le(view.subspan(17, 8), page_id);
    store_le(view.subspan(25, 4), static_cast<std::uint32_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), record.begin() + record_fixed_size);
    // O CRC cobre do lsn ao fim do payload (tudo, menos os 4 bytes do próprio CRC).
    const auto crc = crc32(view.subspan(0, record_fixed_size + payload.size()));
    store_le(view.subspan(record_fixed_size + payload.size(), 4), crc);

    if (auto written = sink_->write_at(write_offset_, record); !written) {
        return std::unexpected(written.error());
    }
    write_offset_ += record.size();
    ++next_lsn_;
    return {};
}

Result<void> Wal::append_begin(std::uint64_t tx_id) {
    return append(WalRecordType::begin, tx_id, 0, {});
}

Result<void> Wal::append_page_image(std::uint64_t tx_id, storage::PageId page_id,
                                    std::span<const std::byte> page) {
    if (page.size() != storage::page_size) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "WAL page image must be a full page"});
    }
    return append(WalRecordType::page_image, tx_id, page_id.value, page);
}

Result<void> Wal::append_commit(std::uint64_t tx_id) {
    return append(WalRecordType::commit, tx_id, 0, {});
}

Result<void> Wal::sync() { return sink_->sync(); }

Result<std::vector<WalRecord>> Wal::read_all(const std::filesystem::path& path) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        return std::unexpected(io_error("could not read WAL size: " + size_error.message()));
    }
    if (size < wal_header_size) {
        return std::unexpected(Error{ErrorCode::corrupt_file, "WAL is smaller than its header"});
    }

    auto file = storage::NativeFile::open(path, storage::NativeFile::Mode::open_existing);
    if (!file) {
        return std::unexpected(file.error());
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (auto read = file->read_at(0, bytes); !read) {
        return std::unexpected(read.error());
    }

    // Valida o cabeçalho (magic, versão, page_size).
    if (!std::equal(wal_magic.begin(), wal_magic.end(), bytes.begin())) {
        return std::unexpected(Error{ErrorCode::invalid_file_format, "file is not a moDb WAL"});
    }
    const std::span<const std::byte> header{bytes.data(), wal_header_size};
    if (load_le<std::uint16_t>(header.subspan(4, 2)) != wal_version) {
        return std::unexpected(
            Error{ErrorCode::incompatible_format_version, "unsupported WAL version"});
    }
    if (load_le<std::uint32_t>(header.subspan(8, 4)) != storage::page_size) {
        return std::unexpected(Error{ErrorCode::corrupt_file, "WAL page size mismatch"});
    }

    std::vector<WalRecord> records;
    std::size_t offset = wal_header_size;
    const std::size_t total = bytes.size();
    while (offset + record_fixed_size <= total) {
        const std::span<const std::byte> fixed{bytes.data() + offset, record_fixed_size};
        const auto length = load_le<std::uint32_t>(fixed.subspan(25, 4));
        const std::size_t record_size = record_fixed_size + length + record_crc_size;
        // Registro truncado no meio: fim lógico do log.
        if (offset + record_size > total) {
            break;
        }
        const std::span<const std::byte> full{bytes.data() + offset, record_size};
        const auto stored_crc =
            load_le<std::uint32_t>(full.subspan(record_fixed_size + length, 4));
        // CRC inválido: fim lógico do log (tudo depois é descartado).
        if (crc32(full.subspan(0, record_fixed_size + length)) != stored_crc) {
            break;
        }
        WalRecord record;
        record.lsn = load_le<std::uint64_t>(full.subspan(0, 8));
        record.tx_id = load_le<std::uint64_t>(full.subspan(8, 8));
        record.type = static_cast<WalRecordType>(std::to_integer<std::uint8_t>(full[16]));
        record.page_id = load_le<std::uint64_t>(full.subspan(17, 8));
        record.payload.assign(full.begin() + record_fixed_size,
                              full.begin() + record_fixed_size + length);
        records.push_back(std::move(record));
        offset += record_size;
    }
    return records;
}

} // namespace modb::tx
