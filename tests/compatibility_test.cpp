// Matriz de compatibilidade formato/protocolo (Fase 10E).

#include "modb/compatibility.hpp"
#include "modb/error.hpp"
#include "modb/net/protocol.hpp"
#include "modb/storage/page_file.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

using namespace modb;
using namespace modb::net;
using namespace modb::storage;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-compat-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void overwrite_u16_le(const std::filesystem::path& path, std::size_t offset, std::uint16_t value) {
    std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
    file.seekp(static_cast<std::streamoff>(offset));
    const unsigned char bytes[2]{static_cast<unsigned char>(value & 0xffU),
                                 static_cast<unsigned char>((value >> 8U) & 0xffU)};
    file.write(reinterpret_cast<const char*>(bytes), 2);
}

std::vector<std::byte> wrap_hello_payload(std::span<const std::byte> payload) {
    BinaryWriter frame;
    frame.write_u32(static_cast<std::uint32_t>(1u + payload.size()));
    frame.write_u8(static_cast<std::uint8_t>(MessageType::hello));
    frame.write_bytes(payload);
    return std::move(frame).take();
}

void test_wire_codec(TestSuite& suite) {
    suite.check(from_wire_u16(1) == CompatibilityVersion{1, 0}, "legacy u16 1 → 1.0");
    suite.check(to_wire_u16({1, 0}) == 1, "1.0 → legacy wire 1");
    suite.check(from_wire_u16(0x0102) == CompatibilityVersion{1, 2}, "packed 0x0102 → 1.2");
    suite.check(to_wire_u16({1, 2}) == 0x0102, "1.2 → packed wire");
}

void test_ensure_readable(TestSuite& suite) {
    suite.check(
        ensure_readable({1, 0}, {1, 0}, ErrorCode::incompatible_format_version, "t").has_value(),
        "same 1.0 is readable");
    suite.check(
        ensure_readable({1, 0}, {1, 1}, ErrorCode::incompatible_format_version, "t").has_value(),
        "older minor is readable by newer reader");
    suite.check_error(ensure_readable({1, 2}, {1, 1}, ErrorCode::incompatible_format_version, "t"),
                      ErrorCode::incompatible_format_version, "future minor is refused");
    suite.check_error(ensure_readable({2, 0}, {1, 0}, ErrorCode::incompatible_format_version, "t"),
                      ErrorCode::incompatible_format_version, "other major is refused");
}

void test_negotiate_protocol(TestSuite& suite) {
    auto ok = negotiate_protocol_version({1, 0}, {1, 0});
    suite.check(ok.has_value() && *ok == CompatibilityVersion{1, 0}, "1.0 ∩ 1.0 → 1.0");
    auto mixed = negotiate_protocol_version({1, 3}, {1, 1});
    suite.check(mixed.has_value() && *mixed == CompatibilityVersion{1, 1},
                "minor is min(client, server)");
    suite.check_error(negotiate_protocol_version({2, 0}, {1, 0}),
                      ErrorCode::incompatible_protocol_version, "major mismatch refused");
}

void test_superblock_major_refused(TestSuite& suite) {
    TemporaryDatabase db{"major"};
    {
        auto file = PageFile::create(db.path());
        suite.check(file.has_value(), "compat fixture created");
    }
    overwrite_u16_le(db.path(), 4, 2);
    suite.check_error(PageFile::open(db.path()), ErrorCode::incompatible_format_version,
                      "format major 2 is refused");
}

void test_superblock_future_minor_refused(TestSuite& suite) {
    TemporaryDatabase db{"minor"};
    {
        auto file = PageFile::create(db.path());
        suite.check(file.has_value(), "compat minor fixture created");
    }
    overwrite_u16_le(db.path(), 4, to_wire_u16({1, 1}));
    suite.check_error(PageFile::open(db.path()), ErrorCode::incompatible_format_version,
                      "format minor 1 is refused by reader 1.0");
}

void test_hello_legacy_without_minor(TestSuite& suite) {
    BinaryWriter payload;
    payload.write_u16(1);
    payload.write_u32(2);
    const char name[] = {'d', 'b'};
    payload.write_bytes(std::as_bytes(std::span{name}));
    payload.write_u8(1);
    payload.write_u8(static_cast<std::uint8_t>(Compression::none));
    auto frame = wrap_hello_payload(std::move(payload).take());
    auto decoded = decode_message(frame);
    suite.check(decoded.has_value(), "legacy Hello without minor decodes");
    if (!decoded) {
        return;
    }
    const auto* hello = std::get_if<Hello>(&*decoded);
    suite.check(hello != nullptr && hello->version == 1 && hello->minor == 0 &&
                    hello->database_name == "db",
                "legacy Hello defaults minor to 0");
}

void test_hello_ignores_trailing_extension(TestSuite& suite) {
    BinaryWriter payload;
    payload.write_u16(1);
    payload.write_u32(2);
    const char name[] = {'d', 'b'};
    payload.write_bytes(std::as_bytes(std::span{name}));
    payload.write_u8(1);
    payload.write_u8(static_cast<std::uint8_t>(Compression::none));
    payload.write_u16(0);   // minor
    payload.write_u8(0xAB); // extensão ignorável
    payload.write_u8(0xCD);
    auto frame = wrap_hello_payload(std::move(payload).take());
    auto decoded = decode_message(frame);
    suite.check(decoded.has_value(), "Hello with ignorable trailing extension decodes");
    if (!decoded) {
        return;
    }
    const auto* hello = std::get_if<Hello>(&*decoded);
    suite.check(hello != nullptr && hello->minor == 0, "trailing extension does not change minor");
}

void test_hello_round_trip_with_minor(TestSuite& suite) {
    Hello hello{.version = protocol_major,
                .minor = protocol_minor,
                .database_name = "shop",
                .accepted_codecs = {Compression::none, Compression::rle}};
    auto encoded = encode_message(hello);
    suite.check(encoded.has_value(), "Hello with minor encodes");
    if (!encoded) {
        return;
    }
    auto decoded = decode_message(*encoded);
    suite.check(decoded.has_value() && std::get_if<Hello>(&*decoded) != nullptr &&
                    *std::get_if<Hello>(&*decoded) == hello,
                "Hello with minor round-trips");
}

} // namespace

int main() {
    TestSuite suite;
    test_wire_codec(suite);
    test_ensure_readable(suite);
    test_negotiate_protocol(suite);
    test_superblock_major_refused(suite);
    test_superblock_future_minor_refused(suite);
    test_hello_legacy_without_minor(suite);
    test_hello_ignores_trailing_extension(suite);
    test_hello_round_trip_with_minor(suite);
    return suite.finish();
}
