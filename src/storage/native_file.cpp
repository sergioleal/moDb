// Importa a interface pública da classe.
#include "modb/storage/native_file.hpp"

// Disponibiliza std::exchange para os movimentos.
#include <utility>
// Disponibiliza std::system_category/generic_category para traduzir erros do SO.
#include <system_error>
// Disponibiliza std::string para as mensagens de erro.
#include <string>

// Cada plataforma traz apenas os cabeçalhos de que precisa.
#ifdef _WIN32
// Reduz a superfície de windows.h e evita macros min/max, sem redefinir o que o
// toolchain já possa ter definido.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
// I/O posicional e fechamento no POSIX.
#include <fcntl.h>
#include <unistd.h>
#endif

namespace modb::storage {
namespace {

// Cria um erro de I/O anexando a descrição do SO ao contexto informado.
Error io_error(const std::string& context, int system_code) {
    // Traduz o código nativo para texto legível na plataforma atual.
    const auto detail = std::system_category().message(system_code);
    return Error{ErrorCode::io_error, context + ": " + detail};
}

} // namespace

#ifdef _WIN32

// Move o HANDLE e deixa a origem sem descritor.
NativeFile::NativeFile(NativeFile&& other) noexcept
    : handle_{std::exchange(other.handle_, nullptr)} {}

// Fecha o descritor atual antes de assumir o do outro objeto.
NativeFile& NativeFile::operator=(NativeFile&& other) noexcept {
    if (this != &other) {
        static_cast<void>(close());
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

NativeFile::~NativeFile() {
    static_cast<void>(close());
}

bool NativeFile::is_open() const noexcept {
    return handle_ != nullptr;
}

Result<NativeFile> NativeFile::open(const std::filesystem::path& path, Mode mode) {
    // Cria falha se já existir; do contrário exige um arquivo existente.
    const DWORD disposition = (mode == Mode::create_new) ? CREATE_NEW : OPEN_EXISTING;
    // Abre com leitura e escrita, permitindo apenas leitura concorrente.
    const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ, nullptr, disposition,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    // INVALID_HANDLE_VALUE indica falha; o motivo vem de GetLastError.
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(
            io_error("could not open file: " + path.string(), static_cast<int>(::GetLastError())));
    }
    // Transfere o HANDLE para um NativeFile pronto.
    NativeFile file;
    file.handle_ = handle;
    return file;
}

Result<void> NativeFile::read_at(std::uint64_t offset, std::span<std::byte> destination) {
    // Percorre o buffer em blocos até completar toda a leitura pedida.
    std::size_t done = 0;
    while (done < destination.size()) {
        // Posiciona esta leitura via OVERLAPPED (offset absoluto, handle síncrono).
        OVERLAPPED overlapped{};
        const std::uint64_t at = offset + done;
        overlapped.Offset = static_cast<DWORD>(at & 0xFFFFFFFFULL);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32U);
        // Limita cada chamada ao teto de DWORD sem estourar o restante.
        const std::size_t remaining = destination.size() - done;
        const DWORD chunk =
            static_cast<DWORD>(remaining > 0x40000000ULL ? 0x40000000ULL : remaining);
        DWORD read = 0;
        // ReadFile falso é erro de I/O; read zero antes do fim é arquivo truncado.
        if (::ReadFile(handle_, destination.data() + done, chunk, &read, &overlapped) == FALSE) {
            return std::unexpected(
                io_error("could not read from file", static_cast<int>(::GetLastError())));
        }
        if (read == 0) {
            return std::unexpected(Error{ErrorCode::corrupt_file, "unexpected end of file"});
        }
        done += read;
    }
    return {};
}

Result<void> NativeFile::write_at(std::uint64_t offset, std::span<const std::byte> source) {
    // Percorre o buffer em blocos até gravar tudo.
    std::size_t done = 0;
    while (done < source.size()) {
        // Posiciona esta escrita via OVERLAPPED (offset absoluto).
        OVERLAPPED overlapped{};
        const std::uint64_t at = offset + done;
        overlapped.Offset = static_cast<DWORD>(at & 0xFFFFFFFFULL);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32U);
        const std::size_t remaining = source.size() - done;
        const DWORD chunk =
            static_cast<DWORD>(remaining > 0x40000000ULL ? 0x40000000ULL : remaining);
        DWORD written = 0;
        if (::WriteFile(handle_, source.data() + done, chunk, &written, &overlapped) == FALSE) {
            return std::unexpected(
                io_error("could not write to file", static_cast<int>(::GetLastError())));
        }
        done += written;
    }
    return {};
}

Result<void> NativeFile::sync() {
    // Força o driver a persistir o cache do arquivo no dispositivo físico.
    if (::FlushFileBuffers(handle_) == FALSE) {
        return std::unexpected(
            io_error("could not flush file to disk", static_cast<int>(::GetLastError())));
    }
    return {};
}

Result<void> NativeFile::close() {
    // Fechar um objeto já fechado é um no-op silencioso.
    if (handle_ == nullptr) {
        return {};
    }
    // Zera o campo antes de reportar para o destrutor nunca fechar duas vezes.
    const HANDLE handle = std::exchange(handle_, nullptr);
    if (::CloseHandle(handle) == FALSE) {
        return std::unexpected(
            io_error("could not close file", static_cast<int>(::GetLastError())));
    }
    return {};
}

#else

// Move o descritor e deixa a origem sem fd.
NativeFile::NativeFile(NativeFile&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

// Fecha o descritor atual antes de assumir o do outro objeto.
NativeFile& NativeFile::operator=(NativeFile&& other) noexcept {
    if (this != &other) {
        static_cast<void>(close());
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

NativeFile::~NativeFile() {
    static_cast<void>(close());
}

bool NativeFile::is_open() const noexcept {
    return fd_ >= 0;
}

Result<NativeFile> NativeFile::open(const std::filesystem::path& path, Mode mode) {
    // Leitura e escrita; criação exclusiva quando o modo é create_new.
    int flags = O_RDWR;
    if (mode == Mode::create_new) {
        flags |= O_CREAT | O_EXCL;
    }
    // 0644: dono lê/escreve, demais apenas leem.
    const int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        return std::unexpected(io_error("could not open file: " + path.string(), errno));
    }
    NativeFile file;
    file.fd_ = fd;
    return file;
}

Result<void> NativeFile::read_at(std::uint64_t offset, std::span<std::byte> destination) {
    // Percorre o buffer em blocos até completar a leitura.
    std::size_t done = 0;
    while (done < destination.size()) {
        const ::ssize_t read = ::pread(fd_, destination.data() + done, destination.size() - done,
                                       static_cast<::off_t>(offset + done));
        // Negativo é erro; zero antes do fim é arquivo truncado.
        if (read < 0) {
            return std::unexpected(io_error("could not read from file", errno));
        }
        if (read == 0) {
            return std::unexpected(Error{ErrorCode::corrupt_file, "unexpected end of file"});
        }
        done += static_cast<std::size_t>(read);
    }
    return {};
}

Result<void> NativeFile::write_at(std::uint64_t offset, std::span<const std::byte> source) {
    // Percorre o buffer em blocos até gravar tudo.
    std::size_t done = 0;
    while (done < source.size()) {
        const ::ssize_t written = ::pwrite(fd_, source.data() + done, source.size() - done,
                                           static_cast<::off_t>(offset + done));
        if (written < 0) {
            return std::unexpected(io_error("could not write to file", errno));
        }
        done += static_cast<std::size_t>(written);
    }
    return {};
}

Result<void> NativeFile::sync() {
    // fsync força a persistência do arquivo no dispositivo.
    if (::fsync(fd_) != 0) {
        return std::unexpected(io_error("could not flush file to disk", errno));
    }
    return {};
}

Result<void> NativeFile::close() {
    // Fechar um objeto já fechado é um no-op silencioso.
    if (fd_ < 0) {
        return {};
    }
    const int fd = std::exchange(fd_, -1);
    if (::close(fd) != 0) {
        return std::unexpected(io_error("could not close file", errno));
    }
    return {};
}

#endif

} // namespace modb::storage
