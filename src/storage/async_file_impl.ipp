// Implementação compartilhada de AsyncFile (incluída pelos backends de plataforma).

#include "modb/storage/async_file.hpp"

#include <utility>

namespace modb::storage {

AsyncFile::AsyncFile(AsyncFile&& other) noexcept
    : file_{std::move(other.file_)},
      backend_{other.backend_},
      fallback_reason_{std::move(other.fallback_reason_)},
      max_inflight_{other.max_inflight_},
      queue_{std::move(other.queue_)} {
    other.backend_ = IoBackend::sync_fallback;
    other.max_inflight_ = 64;
}

AsyncFile& AsyncFile::operator=(AsyncFile&& other) noexcept {
    if (this != &other) {
        static_cast<void>(close());
        file_ = std::move(other.file_);
        backend_ = other.backend_;
        fallback_reason_ = std::move(other.fallback_reason_);
        max_inflight_ = other.max_inflight_;
        queue_ = std::move(other.queue_);
        other.backend_ = IoBackend::sync_fallback;
        other.max_inflight_ = 64;
    }
    return *this;
}

AsyncFile::~AsyncFile() {
    static_cast<void>(cancel_all());
    static_cast<void>(close());
}

bool AsyncFile::is_open() const noexcept {
    return file_.is_open();
}

Result<void> AsyncFile::enqueue(Op op) {
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

Result<void> AsyncFile::apply(const Op& op) {
    switch (op.kind) {
    case OpKind::write:
        return file_.write_at(op.offset, op.write_bytes);
    case OpKind::read: {
        if (op.read_ptr == nullptr || op.read_size == 0) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "AsyncFile read destination empty"});
        }
        return file_.read_at(op.offset, std::span<std::byte>{op.read_ptr, op.read_size});
    }
    case OpKind::sync:
        return file_.sync();
    }
    return std::unexpected(Error{ErrorCode::invalid_argument, "AsyncFile unknown op"});
}

Result<void> AsyncFile::submit_write_at(std::uint64_t offset,
                                        std::span<const std::byte> source) {
    Op op;
    op.kind = OpKind::write;
    op.offset = offset;
    op.write_bytes.assign(source.begin(), source.end());
    return enqueue(std::move(op));
}

Result<void> AsyncFile::submit_read_at(std::uint64_t offset, std::span<std::byte> destination) {
    Op op;
    op.kind = OpKind::read;
    op.offset = offset;
    op.read_ptr = destination.data();
    op.read_size = destination.size();
    return enqueue(std::move(op));
}

Result<void> AsyncFile::submit_sync() {
    Op op;
    op.kind = OpKind::sync;
    return enqueue(std::move(op));
}

Result<void> AsyncFile::drain() {
    if (!is_open()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "AsyncFile is not open"});
    }
    for (const Op& op : queue_) {
        if (auto status = apply(op); !status) {
            return status;
        }
    }
    queue_.clear();
    return {};
}

Result<void> AsyncFile::cancel_all() {
    queue_.clear();
    return {};
}

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

Result<void> AsyncFile::close() {
    if (auto status = cancel_all(); !status) {
        return status;
    }
    return file_.close();
}

Result<AsyncFile> AsyncFile::open(const std::filesystem::path& path, Mode mode,
                                  AsyncFileOptions options) {
    auto native = NativeFile::open(path, mode);
    if (!native) {
        return std::unexpected(native.error());
    }

    IoBackend backend = IoBackend::sync_fallback;
    std::string reason;
#if defined(MODB_ASYNC_FILE_WINDOWS)
    // IOCP completo fica para endurecimento; MinGW usa fallback observável.
    backend = IoBackend::sync_fallback;
    reason = "windows: IOCP not enabled in this build; using NativeFile sync_fallback";
#elif defined(MODB_ASYNC_FILE_LINUX)
    backend = IoBackend::sync_fallback;
    reason = "linux: liburing/io_uring not linked; using NativeFile sync_fallback";
#else
    reason = "platform: async backend unavailable; using NativeFile sync_fallback";
#endif

    if (options.require_async && backend == IoBackend::sync_fallback) {
        return std::unexpected(Error{ErrorCode::io_error,
                                     "AsyncFile require_async but only sync_fallback available: " +
                                         reason});
    }

    AsyncFile file;
    file.file_ = std::move(*native);
    file.backend_ = backend;
    file.fallback_reason_ = std::move(reason);
    file.max_inflight_ = options.max_inflight == 0 ? 1 : options.max_inflight;
    return file;
}

} // namespace modb::storage
