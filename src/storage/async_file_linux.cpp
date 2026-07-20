#include "modb/storage/async_file.hpp"

#ifndef _WIN32

#include <aio.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace modb::storage {
namespace {

Error io_error(const std::string& context, int code) {
    return Error{ErrorCode::io_error, context + ": " + std::generic_category().message(code)};
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
        aiocb cb{};
        bool submitted{};
        bool done{};
    };

    Impl(int fd, std::size_t max_inflight) noexcept
        : fd_{fd}, max_inflight_{max_inflight == 0 ? 1 : max_inflight} {}

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
                if (auto status = aio_sync(); !status) {
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
        if (fd_ < 0) {
            return {};
        }
        const int fd = std::exchange(fd_, -1);
        if (::close(fd) != 0) {
            return std::unexpected(io_error("could not close async file", errno));
        }
        return {};
    }

    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }
    [[nodiscard]] IoBackend backend() const noexcept { return IoBackend::posix_aio; }
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

    void prepare_cb(Op& op) {
        op.cb = aiocb{};
        op.cb.aio_fildes = fd_;
        op.cb.aio_offset = static_cast<off_t>(op.offset);
        if (op.kind == OpKind::write) {
            op.cb.aio_buf = op.write_bytes.data();
            op.cb.aio_nbytes = op.write_bytes.size();
        } else {
            op.cb.aio_buf = op.read_ptr;
            op.cb.aio_nbytes = op.read_size;
        }
        op.submitted = false;
        op.done = false;
    }

    [[nodiscard]] Result<void> submit_group(std::size_t count) {
        if (count == 0) {
            return {};
        }
        for (std::size_t i = 0; i < count; ++i) {
            Op& op = queue_[i];
            const auto size = op.kind == OpKind::write ? op.write_bytes.size() : op.read_size;
            if (size == 0) {
                op.done = true;
                continue;
            }
            prepare_cb(op);
            const int rc = op.kind == OpKind::write ? ::aio_write(&op.cb) : ::aio_read(&op.cb);
            if (rc != 0) {
                cancel_submitted(count);
                return std::unexpected(io_error("could not submit async file operation", errno));
            }
            op.submitted = true;
        }
        return wait_group(count);
    }

    void cancel_submitted(std::size_t count) noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            if (queue_[i].submitted && !queue_[i].done) {
                static_cast<void>(::aio_cancel(fd_, &queue_[i].cb));
            }
        }
    }

    [[nodiscard]] Result<void> wait_group(std::size_t count) {
        std::size_t remaining = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (queue_[i].submitted && !queue_[i].done) {
                ++remaining;
            }
        }

        while (remaining > 0) {
            std::vector<const aiocb*> pending;
            pending.reserve(remaining);
            for (std::size_t i = 0; i < count; ++i) {
                if (queue_[i].submitted && !queue_[i].done) {
                    pending.push_back(&queue_[i].cb);
                }
            }

            int rc = 0;
            do {
                rc = ::aio_suspend(pending.data(), static_cast<int>(pending.size()), nullptr);
            } while (rc != 0 && errno == EINTR);
            if (rc != 0) {
                cancel_submitted(count);
                return std::unexpected(io_error("could not wait for async file operation", errno));
            }

            for (std::size_t i = 0; i < count; ++i) {
                Op& op = queue_[i];
                if (!op.submitted || op.done) {
                    continue;
                }
                const int error = ::aio_error(&op.cb);
                if (error == EINPROGRESS) {
                    continue;
                }
                if (error != 0) {
                    cancel_submitted(count);
                    return std::unexpected(io_error("async file operation failed", error));
                }
                const auto transferred = ::aio_return(&op.cb);
                const auto expected = op.kind == OpKind::write ? op.write_bytes.size() : op.read_size;
                if (transferred < 0) {
                    return std::unexpected(io_error("async file operation return failed", errno));
                }
                if (static_cast<std::size_t>(transferred) != expected) {
                    return std::unexpected(
                        Error{ErrorCode::corrupt_file, "short async file transfer"});
                }
                op.done = true;
                --remaining;
            }
        }
        return {};
    }

    [[nodiscard]] Result<void> aio_sync() {
        aiocb cb{};
        cb.aio_fildes = fd_;
        if (::aio_fsync(O_SYNC, &cb) != 0) {
            return std::unexpected(io_error("could not submit async file sync", errno));
        }
        const aiocb* pending[] = {&cb};
        int rc = 0;
        do {
            rc = ::aio_suspend(pending, 1, nullptr);
        } while (rc != 0 && errno == EINTR);
        if (rc != 0) {
            return std::unexpected(io_error("could not wait for async file sync", errno));
        }
        const int error = ::aio_error(&cb);
        if (error != 0) {
            return std::unexpected(io_error("async file sync failed", error));
        }
        if (::aio_return(&cb) != 0) {
            return std::unexpected(io_error("async file sync return failed", errno));
        }
        return {};
    }

    int fd_{-1};
    std::size_t max_inflight_{64};
    std::vector<Op> queue_{};
};

AsyncFile::AsyncFile(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
AsyncFile::AsyncFile(AsyncFile&&) noexcept = default;
AsyncFile& AsyncFile::operator=(AsyncFile&&) noexcept = default;
AsyncFile::~AsyncFile() = default;

Result<AsyncFile> AsyncFile::open(const std::filesystem::path& path, Mode mode,
                                  AsyncFileOptions options) {
    int flags = O_RDWR;
    if (mode == Mode::create_new) {
        flags |= O_CREAT | O_EXCL;
    }
    const int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        return std::unexpected(io_error("could not open async file: " + path.string(), errno));
    }

    (void)options.require_async;
    return AsyncFile{std::make_unique<Impl>(fd, options.max_inflight)};
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
