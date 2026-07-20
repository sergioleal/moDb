#include "modb/storage/async_file.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace modb::storage {
namespace {

Error io_error(const std::string& context, DWORD code) {
    return Error{ErrorCode::io_error,
                 context + ": " + std::system_category().message(static_cast<int>(code))};
}

} // namespace

class AsyncFile::Impl {
public:
    enum class OpKind : std::uint8_t { read, write, sync };

    struct Op {
        OpKind kind{OpKind::sync};
        std::uint64_t offset{};
        std::vector<std::byte> write_bytes{};
        std::byte* read_ptr{};
        std::size_t read_size{};
        OVERLAPPED overlapped{};
    };

    Impl(HANDLE file, HANDLE port, std::size_t max_inflight) noexcept
        : file_{file}, port_{port}, max_inflight_{max_inflight == 0 ? 1 : max_inflight} {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() { static_cast<void>(close()); }

    [[nodiscard]] Result<void> submit_write_at(std::uint64_t offset,
                                               std::span<const std::byte> source) {
        Op op;
        op.kind = OpKind::write;
        op.offset = offset;
        op.write_bytes.assign(source.begin(), source.end());
        return enqueue(std::move(op));
    }

    [[nodiscard]] Result<void> submit_read_at(std::uint64_t offset,
                                              std::span<std::byte> destination) {
        Op op;
        op.kind = OpKind::read;
        op.offset = offset;
        op.read_ptr = destination.data();
        op.read_size = destination.size();
        return enqueue(std::move(op));
    }

    [[nodiscard]] Result<void> submit_sync() {
        Op op;
        op.kind = OpKind::sync;
        return enqueue(std::move(op));
    }

    [[nodiscard]] Result<void> drain() {
        while (!queue_.empty()) {
            if (queue_.front().kind == OpKind::sync) {
                if (auto status = flush(); !status) {
                    return status;
                }
                queue_.erase(queue_.begin());
                continue;
            }

            std::size_t group_size = 0;
            while (group_size < queue_.size() && queue_[group_size].kind != OpKind::sync) {
                ++group_size;
            }
            if (auto status = submit_group(group_size); !status) {
                return status;
            }
            queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(group_size));
        }
        return {};
    }

    [[nodiscard]] Result<void> cancel_all() {
        queue_.clear();
        return {};
    }

    [[nodiscard]] Result<void> close() {
        queue_.clear();
        Error close_error{ErrorCode::io_error, ""};
        bool failed = false;
        if (file_ != INVALID_HANDLE_VALUE) {
            const HANDLE file = std::exchange(file_, INVALID_HANDLE_VALUE);
            if (::CloseHandle(file) == FALSE) {
                close_error = io_error("could not close async file", ::GetLastError());
                failed = true;
            }
        }
        if (port_ != nullptr) {
            const HANDLE port = std::exchange(port_, nullptr);
            if (::CloseHandle(port) == FALSE && !failed) {
                close_error = io_error("could not close IO completion port", ::GetLastError());
                failed = true;
            }
        }
        if (failed) {
            return std::unexpected(close_error);
        }
        return {};
    }

    [[nodiscard]] bool is_open() const noexcept { return file_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] IoBackend backend() const noexcept { return IoBackend::iocp; }
    [[nodiscard]] std::string_view fallback_reason() const noexcept { return {}; }
    [[nodiscard]] std::size_t inflight() const noexcept { return queue_.size(); }
    [[nodiscard]] std::size_t max_inflight() const noexcept { return max_inflight_; }

private:
    [[nodiscard]] Result<void> enqueue(Op op) {
        if (!is_open()) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "AsyncFile is not open"});
        }
        if (queue_.size() >= max_inflight_) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "AsyncFile max_inflight exceeded"});
        }
        queue_.push_back(std::move(op));
        return {};
    }

    [[nodiscard]] Result<void> flush() {
        if (::FlushFileBuffers(file_) == FALSE) {
            return std::unexpected(io_error("could not flush async file", ::GetLastError()));
        }
        return {};
    }

    [[nodiscard]] Result<void> submit_group(std::size_t count) {
        if (count == 0) {
            return {};
        }
        std::size_t submitted = 0;
        for (std::size_t i = 0; i < count; ++i) {
            Op& op = queue_[i];
            const auto size = op.kind == OpKind::write ? op.write_bytes.size() : op.read_size;
            if (size == 0) {
                continue;
            }
            if (size > std::numeric_limits<DWORD>::max()) {
                return std::unexpected(
                    Error{ErrorCode::value_too_large, "AsyncFile operation exceeds DWORD"});
            }
            op.overlapped = OVERLAPPED{};
            op.overlapped.Offset = static_cast<DWORD>(op.offset & 0xFFFFFFFFULL);
            op.overlapped.OffsetHigh = static_cast<DWORD>(op.offset >> 32U);

            const DWORD bytes = static_cast<DWORD>(size);
            const BOOL ok = op.kind == OpKind::write
                                ? ::WriteFile(file_, op.write_bytes.data(), bytes, nullptr,
                                              &op.overlapped)
                                : ::ReadFile(file_, op.read_ptr, bytes, nullptr,
                                             &op.overlapped);
            if (ok == FALSE) {
                const DWORD error = ::GetLastError();
                if (error != ERROR_IO_PENDING) {
                    static_cast<void>(::CancelIoEx(file_, nullptr));
                    return std::unexpected(io_error("could not submit async file operation", error));
                }
            }
            ++submitted;
        }

        for (std::size_t completed = 0; completed < submitted; ++completed) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            LPOVERLAPPED overlapped = nullptr;
            if (::GetQueuedCompletionStatus(port_, &bytes, &key, &overlapped, INFINITE) == FALSE) {
                const DWORD error = ::GetLastError();
                static_cast<void>(::CancelIoEx(file_, nullptr));
                return std::unexpected(io_error("async file operation failed", error));
            }
            const Op* finished = nullptr;
            for (std::size_t i = 0; i < count; ++i) {
                if (&queue_[i].overlapped == overlapped) {
                    finished = &queue_[i];
                    break;
                }
            }
            if (finished == nullptr) {
                return std::unexpected(
                    Error{ErrorCode::io_error, "unknown async file completion"});
            }
            const auto expected = finished->kind == OpKind::write ? finished->write_bytes.size()
                                                                  : finished->read_size;
            if (static_cast<std::size_t>(bytes) != expected) {
                return std::unexpected(
                    Error{ErrorCode::corrupt_file, "short async file transfer"});
            }
        }
        return {};
    }

    HANDLE file_{INVALID_HANDLE_VALUE};
    HANDLE port_{nullptr};
    std::size_t max_inflight_{64};
    std::vector<Op> queue_{};
};

AsyncFile::AsyncFile(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
AsyncFile::AsyncFile(AsyncFile&&) noexcept = default;
AsyncFile& AsyncFile::operator=(AsyncFile&&) noexcept = default;
AsyncFile::~AsyncFile() = default;

Result<AsyncFile> AsyncFile::open(const std::filesystem::path& path, Mode mode,
                                  AsyncFileOptions options) {
    const DWORD disposition = (mode == Mode::create_new) ? CREATE_NEW : OPEN_EXISTING;
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, disposition,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::unexpected(io_error("could not open async file: " + path.string(),
                                        ::GetLastError()));
    }

    const HANDLE port = ::CreateIoCompletionPort(file, nullptr, 0, 0);
    if (port == nullptr) {
        const DWORD error = ::GetLastError();
        static_cast<void>(::CloseHandle(file));
        return std::unexpected(
            io_error("could not create IO completion port: " + path.string(), error));
    }

    (void)options.require_async;
    return AsyncFile{std::make_unique<Impl>(file, port, options.max_inflight)};
}

Result<void> AsyncFile::submit_write_at(std::uint64_t offset,
                                        std::span<const std::byte> source) {
    return impl_->submit_write_at(offset, source);
}
Result<void> AsyncFile::submit_read_at(std::uint64_t offset, std::span<std::byte> destination) {
    return impl_->submit_read_at(offset, destination);
}
Result<void> AsyncFile::submit_sync() { return impl_->submit_sync(); }
Result<void> AsyncFile::write_at(std::uint64_t offset, std::span<const std::byte> source) {
    if (auto status = submit_write_at(offset, source); !status) {
        return status;
    }
    return drain();
}
Result<void> AsyncFile::read_at(std::uint64_t offset, std::span<std::byte> destination) {
    if (auto status = submit_read_at(offset, destination); !status) {
        return status;
    }
    return drain();
}
Result<void> AsyncFile::sync() {
    if (auto status = submit_sync(); !status) {
        return status;
    }
    return drain();
}
Result<void> AsyncFile::drain() { return impl_ ? impl_->drain() : Result<void>{}; }
Result<void> AsyncFile::cancel_all() { return impl_ ? impl_->cancel_all() : Result<void>{}; }
Result<void> AsyncFile::close() { return impl_ ? impl_->close() : Result<void>{}; }
bool AsyncFile::is_open() const noexcept { return impl_ != nullptr && impl_->is_open(); }
IoBackend AsyncFile::backend() const noexcept {
    return impl_ == nullptr ? IoBackend::sync_fallback : impl_->backend();
}
std::string_view AsyncFile::fallback_reason() const noexcept {
    return impl_ == nullptr ? std::string_view{"not open"} : impl_->fallback_reason();
}
std::size_t AsyncFile::inflight() const noexcept { return impl_ == nullptr ? 0 : impl_->inflight(); }
std::size_t AsyncFile::max_inflight() const noexcept {
    return impl_ == nullptr ? 0 : impl_->max_inflight();
}

} // namespace modb::storage

#endif
