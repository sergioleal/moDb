#include "modb/net/shm_ring.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define MODB_CPU_RELAX() _mm_pause()
#else
#define MODB_CPU_RELAX() std::atomic_signal_fence(std::memory_order_seq_cst)
#endif

namespace modb::net::shm {

namespace {

constexpr char k_magic[8] = {'M', 'D', 'B', 'S', 'H', 'M', '0', '1'};

Error io(std::string message) { return Error{ErrorCode::io_error, std::move(message)}; }

std::uint64_t load_pos(const std::byte* p) noexcept {
    return std::atomic_ref<std::uint64_t>{*reinterpret_cast<std::uint64_t*>(const_cast<std::byte*>(p))}.load(
        std::memory_order_acquire);
}

void store_pos(std::byte* p, std::uint64_t v) noexcept {
    std::atomic_ref<std::uint64_t>{*reinterpret_cast<std::uint64_t*>(p)}.store(v, std::memory_order_release);
}

std::uint64_t align8(std::uint64_t n) noexcept { return (n + 7u) & ~std::uint64_t{7}; }

std::uint32_t read_u32(const std::byte* p) noexcept {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof v);  // little-endian: as plataformas suportadas
    return v;
}

// Nome único por processo e região.
std::string unique_suffix() {
    static std::atomic<std::uint32_t> counter{0};
    std::random_device rd;
#ifdef _WIN32
    const auto pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    return std::to_string(pid) + "-" + std::to_string(counter.fetch_add(1)) + "-" + std::to_string(rd());
}

} // namespace

Result<std::uint32_t> normalize_ring_bytes(std::uint32_t requested) {
    if (requested == 0) {
        return k_default_ring_bytes;
    }
    if (requested < k_min_ring_bytes || requested > k_max_ring_bytes) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
                                     "shared-memory ring size must be between " + std::to_string(k_min_ring_bytes) +
                                         " and " + std::to_string(k_max_ring_bytes) + " bytes"});
    }
    return static_cast<std::uint32_t>(align8(requested));
}

// --- Region ---------------------------------------------------------------------

Region::Region(Region&& other) noexcept
    : base_{std::exchange(other.base_, nullptr)}, size_{std::exchange(other.size_, 0)},
      ring_bytes_{other.ring_bytes_}, kind_{other.kind_}, name_{std::move(other.name_)},
      owner_{std::exchange(other.owner_, false)}, unlinked_{other.unlinked_},
      handle_{std::exchange(other.handle_, nullptr)}, fd_{std::exchange(other.fd_, -1)} {}

Region& Region::operator=(Region&& other) noexcept {
    if (this != &other) {
        release();
        base_ = std::exchange(other.base_, nullptr);
        size_ = std::exchange(other.size_, 0);
        ring_bytes_ = other.ring_bytes_;
        kind_ = other.kind_;
        name_ = std::move(other.name_);
        owner_ = std::exchange(other.owner_, false);
        unlinked_ = other.unlinked_;
        handle_ = std::exchange(other.handle_, nullptr);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

Region::~Region() { release(); }

void Region::release() noexcept {
    if (owner_) {
        unlink();
    }
#ifdef _WIN32
    if (base_ != nullptr) {
        UnmapViewOfFile(base_);
    }
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
#else
    if (base_ != nullptr) {
        munmap(base_, size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
#endif
    base_ = nullptr;
    handle_ = nullptr;
    fd_ = -1;
    size_ = 0;
}

void Region::unlink() noexcept {
    if (unlinked_ || name_.empty()) {
        return;
    }
    unlinked_ = true;
#ifndef _WIN32
    ::unlink(name_.c_str());
#endif
}

std::uint32_t Region::load_state(std::size_t offset) const noexcept {
    return std::atomic_ref<std::uint32_t>{*reinterpret_cast<std::uint32_t*>(base_ + offset)}.load(
        std::memory_order_acquire);
}

void Region::store_state(std::size_t offset, std::uint32_t value) noexcept {
    std::atomic_ref<std::uint32_t>{*reinterpret_cast<std::uint32_t*>(base_ + offset)}.store(
        value, std::memory_order_release);
}

Result<Region> Region::create(std::uint32_t ring_bytes) {
    auto rb = normalize_ring_bytes(ring_bytes);
    if (!rb) {
        return std::unexpected(rb.error());
    }
    Region region;
    region.ring_bytes_ = *rb;
    region.size_ = k_header_bytes + 2u * static_cast<std::size_t>(*rb);
    region.owner_ = true;
#ifdef _WIN32
    region.kind_ = ShmRegionKind::windows_named_mapping;
    region.name_ = "Local\\modb-shm-" + unique_suffix();
    const auto size64 = static_cast<std::uint64_t>(region.size_);
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        static_cast<DWORD>(size64 >> 32), static_cast<DWORD>(size64 & 0xFFFFFFFFu),
                                        region.name_.c_str());
    if (mapping == nullptr) {
        return std::unexpected(io("CreateFileMapping failed: " + std::to_string(GetLastError())));
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        return std::unexpected(io("shared-memory name collision: " + region.name_));
    }
    region.handle_ = mapping;
    region.base_ = static_cast<std::byte*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, region.size_));
    if (region.base_ == nullptr) {
        return std::unexpected(io("MapViewOfFile failed: " + std::to_string(GetLastError())));
    }
#else
    region.kind_ = ShmRegionKind::file_path;
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::is_directory("/dev/shm", ec) ? std::filesystem::path{"/dev/shm"}
                                                      : std::filesystem::temp_directory_path();
    region.name_ = (dir / ("modb-shm-" + unique_suffix())).string();
    region.fd_ = ::open(region.name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (region.fd_ < 0) {
        return std::unexpected(io("cannot create " + region.name_ + ": errno " + std::to_string(errno)));
    }
    if (::ftruncate(region.fd_, static_cast<off_t>(region.size_)) != 0) {
        return std::unexpected(io("ftruncate failed: errno " + std::to_string(errno)));
    }
    void* p = ::mmap(nullptr, region.size_, PROT_READ | PROT_WRITE, MAP_SHARED, region.fd_, 0);
    if (p == MAP_FAILED) {
        return std::unexpected(io("mmap failed: errno " + std::to_string(errno)));
    }
    region.base_ = static_cast<std::byte*>(p);
#endif
    std::memset(region.base_, 0, k_header_bytes);
    std::memcpy(region.base_, k_magic, sizeof k_magic);
    std::memcpy(region.base_ + k_off_ring_bytes, &region.ring_bytes_, sizeof region.ring_bytes_);
    region.store_state(k_off_server_state, static_cast<std::uint32_t>(ServerState::serving));
    return region;
}

Result<Region> Region::open(ShmRegionKind kind, std::string_view name, std::uint32_t ring_bytes) {
    Region region;
    region.kind_ = kind;
    region.name_ = std::string{name};
    region.ring_bytes_ = ring_bytes;
    region.size_ = k_header_bytes + 2u * static_cast<std::size_t>(ring_bytes);
#ifdef _WIN32
    if (kind != ShmRegionKind::windows_named_mapping) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "server offered a file region on Windows"});
    }
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, region.name_.c_str());
    if (mapping == nullptr) {
        return std::unexpected(io("OpenFileMapping failed: " + std::to_string(GetLastError())));
    }
    region.handle_ = mapping;
    region.base_ = static_cast<std::byte*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, region.size_));
    if (region.base_ == nullptr) {
        return std::unexpected(io("MapViewOfFile failed: " + std::to_string(GetLastError())));
    }
#else
    if (kind != ShmRegionKind::file_path) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "server offered a Windows mapping"});
    }
    region.fd_ = ::open(region.name_.c_str(), O_RDWR);
    if (region.fd_ < 0) {
        return std::unexpected(io("cannot open " + region.name_ + ": errno " + std::to_string(errno)));
    }
    void* p = ::mmap(nullptr, region.size_, PROT_READ | PROT_WRITE, MAP_SHARED, region.fd_, 0);
    if (p == MAP_FAILED) {
        return std::unexpected(io("mmap failed: errno " + std::to_string(errno)));
    }
    region.base_ = static_cast<std::byte*>(p);
#endif
    std::uint32_t stored = 0;
    std::memcpy(&stored, region.base_ + k_off_ring_bytes, sizeof stored);
    if (std::memcmp(region.base_, k_magic, sizeof k_magic) != 0 || stored != ring_bytes) {
        return std::unexpected(Error{ErrorCode::protocol_error, "shared-memory region has a bad header"});
    }
    return region;
}

// --- Ring -----------------------------------------------------------------------

Ring Ring::requests(const Region& region) noexcept {
    return Ring{region.data() + k_header_bytes, region.ring_bytes(), region.data() + k_off_req_tail,
                region.data() + k_off_req_head};
}

Ring Ring::responses(const Region& region) noexcept {
    return Ring{region.data() + k_header_bytes + region.ring_bytes(), region.ring_bytes(),
                region.data() + k_off_resp_tail, region.data() + k_off_resp_head};
}

Result<bool> Ring::try_write(std::span<const std::byte> frame) {
    const auto need = align8(frame.size());
    if (frame.size() < 5 || need > ring_bytes_) {
        return std::unexpected(Error{ErrorCode::value_too_large,
                                     "message of " + std::to_string(frame.size()) +
                                         " bytes does not fit the shared-memory ring of " +
                                         std::to_string(ring_bytes_) + " bytes"});
    }
    const auto head = load_pos(head_);
    auto tail = load_pos(tail_);  // só este produtor escreve tail
    auto offset = tail % ring_bytes_;
    const auto to_end = ring_bytes_ - offset;
    const auto total = need > to_end ? to_end + need : need;
    if (tail + total - head > ring_bytes_) {
        return false;
    }
    if (need > to_end) {
        std::memcpy(data_ + offset, &k_padding_marker, sizeof k_padding_marker);
        tail += to_end;
        offset = 0;
    }
    std::memcpy(data_ + offset, frame.data(), frame.size());
    store_pos(tail_, tail + need);
    return true;
}

Result<std::optional<std::span<const std::byte>>> Ring::peek() {
    auto head = load_pos(head_);  // só este consumidor escreve head
    const auto tail = load_pos(tail_);
    for (;;) {
        if (head == tail) {
            return std::optional<std::span<const std::byte>>{};
        }
        const auto offset = head % ring_bytes_;
        const auto length = read_u32(data_ + offset);
        if (length == k_padding_marker) {
            head += ring_bytes_ - offset;
            store_pos(head_, head);
            continue;
        }
        const auto frame_bytes = 4u + static_cast<std::uint64_t>(length);
        if (length == 0 || length > max_frame_bytes || frame_bytes > ring_bytes_ - offset ||
            head + align8(frame_bytes) > tail) {
            return std::unexpected(Error{ErrorCode::protocol_error, "corrupt shared-memory ring"});
        }
        pending_ = align8(frame_bytes);
        return std::optional<std::span<const std::byte>>{
            std::span<const std::byte>{data_ + offset, static_cast<std::size_t>(frame_bytes)}};
    }
}

void Ring::pop() noexcept {
    if (pending_ == 0) {
        return;
    }
    store_pos(head_, load_pos(head_) + pending_);
    pending_ = 0;
}

// --- Backoff --------------------------------------------------------------------

void Backoff::wait() {
    const auto now = std::chrono::steady_clock::now();
    if (!started_) {
        started_ = true;
        since_ = now;
        sleep_ = std::chrono::microseconds{50};
    }
    const auto waited = now - since_;
    if (waited < k_spin) {
        for (int i = 0; i < 64; ++i) {
            MODB_CPU_RELAX();
        }
        return;
    }
    if (waited < k_yield) {
        std::this_thread::yield();
        return;
    }
#ifdef _WIN32
    // Sleep/sleep_for no Windows arredonda para o tick do sistema (até 15,6 ms);
    // o timer de alta resolução dorme o que se pede.
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                        TIMER_ALL_ACCESS);
    if (timer != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(sleep_.count()) * 10;  // unidades de 100 ns, relativo
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        }
    } else {
        std::this_thread::sleep_for(sleep_);
    }
#else
    std::this_thread::sleep_for(sleep_);
#endif
    sleep_ = std::min(sleep_ * 2, std::chrono::duration_cast<std::chrono::microseconds>(k_max_sleep));
}

} // namespace modb::net::shm
