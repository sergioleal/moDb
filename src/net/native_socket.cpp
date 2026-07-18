#include "modb/net/native_socket.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace modb::net {
namespace {

Error make_io(std::string context, int code) {
    return Error{ErrorCode::io_error, std::move(context) + ": " + std::to_string(code)};
}

Error make_closed(std::string context) {
    return Error{ErrorCode::connection_closed, std::move(context)};
}

#ifdef _WIN32

struct WinsockLifetime {
    WinsockLifetime() {
        WSADATA data{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
        ok_ = (rc == 0);
        startup_code_ = rc;
    }
    ~WinsockLifetime() {
        if (ok_) {
            ::WSACleanup();
        }
    }
    bool ok_{false};
    int startup_code_{0};
};

Result<void> ensure_winsock() {
    static WinsockLifetime runtime;
    if (!runtime.ok_) {
        return std::unexpected(make_io("WSAStartup failed", runtime.startup_code_));
    }
    return {};
}

using SocketHandle = SOCKET;
constexpr SocketHandle kInvalid = INVALID_SOCKET;

int last_error() {
    return ::WSAGetLastError();
}

void close_handle(SocketHandle socket) {
    if (socket != kInvalid) {
        ::closesocket(socket);
    }
}

#else

using SocketHandle = int;
constexpr SocketHandle kInvalid = -1;

int last_error() {
    return errno;
}

void close_handle(SocketHandle socket) {
    if (socket != kInvalid) {
        ::close(socket);
    }
}

Result<void> ensure_winsock() {
    return {};
}

#endif

Result<SocketHandle> create_tcp_socket() {
    if (auto ready = ensure_winsock(); !ready) {
        return std::unexpected(ready.error());
    }
    const SocketHandle socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalid) {
        return std::unexpected(make_io("socket() failed", last_error()));
    }
#ifdef _WIN32
    BOOL yes = TRUE;
    (void)::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                       sizeof(yes));
#else
    int yes = 1;
    (void)::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    return socket;
}

Result<sockaddr_in> resolve_ipv4(std::string_view host, std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    std::string host_copy{host};
    if (::inet_pton(AF_INET, host_copy.c_str(), &address.sin_addr) == 1) {
        return address;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host_copy.c_str(), nullptr, &hints, &result);
    if (rc != 0 || result == nullptr) {
        return std::unexpected(make_io("getaddrinfo failed for " + host_copy, rc));
    }
    const auto* ipv4 = reinterpret_cast<sockaddr_in*>(result->ai_addr);
    address.sin_addr = ipv4->sin_addr;
    ::freeaddrinfo(result);
    return address;
}

} // namespace

#ifdef _WIN32

NativeSocket::NativeSocket(NativeSocket&& other) noexcept
    : socket_{std::exchange(other.socket_, static_cast<std::uintptr_t>(-1))} {}

NativeSocket& NativeSocket::operator=(NativeSocket&& other) noexcept {
    if (this != &other) {
        static_cast<void>(close());
        socket_ = std::exchange(other.socket_, static_cast<std::uintptr_t>(-1));
    }
    return *this;
}

NativeSocket::~NativeSocket() {
    static_cast<void>(close());
}

bool NativeSocket::is_open() const noexcept {
    return socket_ != static_cast<std::uintptr_t>(-1);
}

Result<void> NativeSocket::close() {
    if (!is_open()) {
        return {};
    }
    const SocketHandle handle = static_cast<SocketHandle>(socket_);
    socket_ = static_cast<std::uintptr_t>(-1);
    if (::closesocket(handle) == SOCKET_ERROR) {
        return std::unexpected(make_io("closesocket failed", last_error()));
    }
    return {};
}

Result<NativeSocket> NativeSocket::listen(std::string_view host, std::uint16_t port, int backlog) {
    auto created = create_tcp_socket();
    if (!created) {
        return std::unexpected(created.error());
    }
    SocketHandle handle = *created;
    auto address = resolve_ipv4(host, port);
    if (!address) {
        close_handle(handle);
        return std::unexpected(address.error());
    }
    if (::bind(handle, reinterpret_cast<sockaddr*>(&*address), sizeof(*address)) == SOCKET_ERROR) {
        const int code = last_error();
        close_handle(handle);
        return std::unexpected(make_io("bind failed", code));
    }
    if (::listen(handle, backlog) == SOCKET_ERROR) {
        const int code = last_error();
        close_handle(handle);
        return std::unexpected(make_io("listen failed", code));
    }
    return NativeSocket{static_cast<std::uintptr_t>(handle)};
}

Result<NativeSocket> NativeSocket::connect(std::string_view host, std::uint16_t port) {
    auto created = create_tcp_socket();
    if (!created) {
        return std::unexpected(created.error());
    }
    SocketHandle handle = *created;
    auto address = resolve_ipv4(host, port);
    if (!address) {
        close_handle(handle);
        return std::unexpected(address.error());
    }
    if (::connect(handle, reinterpret_cast<sockaddr*>(&*address), sizeof(*address)) ==
        SOCKET_ERROR) {
        const int code = last_error();
        close_handle(handle);
        return std::unexpected(make_io("connect failed", code));
    }
    return NativeSocket{static_cast<std::uintptr_t>(handle)};
}

Result<NativeSocket> NativeSocket::accept() {
    if (!is_open()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "accept on closed socket"});
    }
    const SocketHandle listener = static_cast<SocketHandle>(socket_);
    const SocketHandle client = ::accept(listener, nullptr, nullptr);
    if (client == kInvalid) {
        return std::unexpected(make_io("accept failed", last_error()));
    }
    return NativeSocket{static_cast<std::uintptr_t>(client)};
}

Result<void> NativeSocket::send_all(std::span<const std::byte> bytes) {
    if (!is_open()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "send on closed socket"});
    }
    const SocketHandle handle = static_cast<SocketHandle>(socket_);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int chunk = static_cast<int>((std::min)(bytes.size() - sent, std::size_t{1} << 20));
        const int rc =
            ::send(handle, reinterpret_cast<const char*>(bytes.data() + sent), chunk, 0);
        if (rc == SOCKET_ERROR) {
            return std::unexpected(make_io("send failed", last_error()));
        }
        if (rc == 0) {
            return std::unexpected(make_closed("peer closed during send"));
        }
        sent += static_cast<std::size_t>(rc);
    }
    return {};
}

Result<void> NativeSocket::recv_exact(std::span<std::byte> destination) {
    if (!is_open()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "recv on closed socket"});
    }
    const SocketHandle handle = static_cast<SocketHandle>(socket_);
    std::size_t got = 0;
    while (got < destination.size()) {
        const int chunk =
            static_cast<int>((std::min)(destination.size() - got, std::size_t{1} << 20));
        const int rc =
            ::recv(handle, reinterpret_cast<char*>(destination.data() + got), chunk, 0);
        if (rc == SOCKET_ERROR) {
            return std::unexpected(make_io("recv failed", last_error()));
        }
        if (rc == 0) {
            return std::unexpected(make_closed("peer closed during recv"));
        }
        got += static_cast<std::size_t>(rc);
    }
    return {};
}

Result<std::uint16_t> NativeSocket::local_port() const {
    if (!is_open()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "local_port on closed socket"});
    }
    sockaddr_in address{};
    int length = sizeof(address);
    if (::getsockname(static_cast<SocketHandle>(socket_), reinterpret_cast<sockaddr*>(&address),
                      &length) == SOCKET_ERROR) {
        return std::unexpected(make_io("getsockname failed", last_error()));
    }
    return ntohs(address.sin_port);
}

#else

NativeSocket::NativeSocket(NativeSocket&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

NativeSocket& NativeSocket::operator=(NativeSocket&& other) noexcept {
    if (this != &other) {
        static_cast<void>(close());
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

NativeSocket::~NativeSocket() {
    static_cast<void>(close());
}

bool NativeSocket::is_open() const noexcept {
    return fd_ != -1;
}

Result<void> NativeSocket::close() {
    if (!is_open()) {
        return {};
    }
    const int handle = fd_;
    fd_ = -1;
    if (::close(handle) != 0) {
        return std::unexpected(make_io("close failed", last_error()));
    }
    return {};
}

Result<NativeSocket> NativeSocket::listen(std::string_view host, std::uint16_t port, int backlog) {
    auto created = create_tcp_socket();
    if (!created) {
        return std::unexpected(created.error());
    }
    SocketHandle handle = *created;
    auto address = resolve_ipv4(host, port);
    if (!address) {
        close_handle(handle);
        return std::unexpected(address.error());
    }
    if (::bind(handle, reinterpret_cast<sockaddr*>(&*address), sizeof(*address)) != 0) {
        const int code = last_error();
        close_handle(handle);
        return std::unexpected(make_io("bind failed", code));
    }
    if (::listen(handle, backlog) != 0) {
        const int code = last_error();
        close_handle(handle);
        return std::unexpected(make_io("listen failed", code));
    }
    return NativeSocket{handle};
}

Result<NativeSocket> NativeSocket::connect(std::string_view host, std::uint16_t port) {
    auto created = create_tcp_socket();
    if (!created) {
        return std::unexpected(created.error());
    }
    SocketHandle handle = *created;
    auto address = resolve_ipv4(host, port);
    if (!address) {
        close_handle(handle);
        return std::unexpected(address.error());
    }
    if (::connect(handle, reinterpret_cast<sockaddr*>(&*address), sizeof(*address)) != 0) {
        const int code = last_error();
        close_handle(handle);
        return std::unexpected(make_io("connect failed", code));
    }
    return NativeSocket{handle};
}

Result<NativeSocket> NativeSocket::accept() {
    if (!is_open()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "accept on closed socket"});
    }
    const SocketHandle client = ::accept(fd_, nullptr, nullptr);
    if (client == kInvalid) {
        return std::unexpected(make_io("accept failed", last_error()));
    }
    return NativeSocket{client};
}

Result<void> NativeSocket::send_all(std::span<const std::byte> bytes) {
    if (!is_open()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "send on closed socket"});
    }
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto chunk = (std::min)(bytes.size() - sent, std::size_t{1} << 20);
        const ssize_t rc = ::send(fd_, bytes.data() + sent, chunk, 0);
        if (rc < 0) {
            return std::unexpected(make_io("send failed", last_error()));
        }
        if (rc == 0) {
            return std::unexpected(make_closed("peer closed during send"));
        }
        sent += static_cast<std::size_t>(rc);
    }
    return {};
}

Result<void> NativeSocket::recv_exact(std::span<std::byte> destination) {
    if (!is_open()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "recv on closed socket"});
    }
    std::size_t got = 0;
    while (got < destination.size()) {
        const auto chunk = (std::min)(destination.size() - got, std::size_t{1} << 20);
        const ssize_t rc = ::recv(fd_, destination.data() + got, chunk, 0);
        if (rc < 0) {
            return std::unexpected(make_io("recv failed", last_error()));
        }
        if (rc == 0) {
            return std::unexpected(make_closed("peer closed during recv"));
        }
        got += static_cast<std::size_t>(rc);
    }
    return {};
}

Result<std::uint16_t> NativeSocket::local_port() const {
    if (!is_open()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "local_port on closed socket"});
    }
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return std::unexpected(make_io("getsockname failed", last_error()));
    }
    return ntohs(address.sin_port);
}

#endif

} // namespace modb::net
