// modb-proxy: acesso remoto a um engine moDb que escuta só localmente
// (ADR-028, PLANO_PROXY X5.5).
//
//   modb-proxy --engine /run/modb/app.sock --port 7474 [--policy passthrough]
//
// Imprime "READY <porta>" quando já aceita clientes; SIGINT/SIGTERM param limpo.

#include "modb/proxy/proxy.hpp"

#include <algorithm>
#include <charconv>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace {

using modb::Error;
using modb::ErrorCode;
using modb::Result;

struct Settings {
    modb::proxy::Options proxy{};
    std::string policy{"passthrough"};
};

Error invalid(std::string message) { return Error{ErrorCode::invalid_argument, std::move(message)}; }

template <typename N>
Result<N> parse_number(std::string_view key, std::string_view text) {
    unsigned long long value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() ||
        value > std::numeric_limits<N>::max()) {
        return std::unexpected(invalid("invalid " + std::string{key} + ": " + std::string{text}));
    }
    return static_cast<N>(value);
}

std::string_view trim(std::string_view s) {
    constexpr std::string_view blanks = " \t\r\n";
    const auto first = s.find_first_not_of(blanks);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(blanks) - first + 1);
}

std::filesystem::path resolve(const std::filesystem::path& base, std::string_view value) {
    std::filesystem::path path{std::string{value}};
    return base.empty() || path.is_absolute() ? path : base / path;
}

Result<std::string> read_secret(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return std::unexpected(invalid("cannot read secret file: " + file.string()));
    }
    std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    return std::string{trim(text)};
}

Result<void> apply_setting(Settings& settings, std::string_view key, std::string_view value,
                           const std::filesystem::path& base = {}) {
    auto& options = settings.proxy;
    auto set_number = [&](auto& field) -> Result<void> {
        auto n = parse_number<std::remove_reference_t<decltype(field)>>(key, value);
        if (!n) {
            return std::unexpected(n.error());
        }
        field = *n;
        return {};
    };
    if (key == "engine") {
        options.engine = resolve(base, value);
    } else if (key == "secret_file") {
        auto secret = read_secret(resolve(base, value));
        if (!secret) {
            return std::unexpected(secret.error());
        }
        options.link_secret = std::move(*secret);
    } else if (key == "name") {
        options.name = std::string{value};
    } else if (key == "host") {
        options.host = std::string{value};
    } else if (key == "port") {
        return set_number(options.port);
    } else if (key == "idle_timeout_ms") {
        return set_number(options.idle_timeout_ms);
    } else if (key == "stream_credit") {
        return set_number(options.stream_credit);
    } else if (key == "reconnect_max_ms") {
        return set_number(options.reconnect_max_ms);
    } else if (key == "compression") {
        if (value == "rle") {
            options.preferred_codec = modb::net::Compression::rle;
        } else if (value == "none") {
            options.preferred_codec = modb::net::Compression::none;
        } else {
            return std::unexpected(invalid("compression must be rle or none"));
        }
    } else if (key == "policy") {
        settings.policy = std::string{value};
    } else {
        return std::unexpected(invalid("unknown setting: " + std::string{key}));
    }
    return {};
}

Result<void> load_config(const std::filesystem::path& file, Settings& settings) {
    std::ifstream in{file};
    if (!in) {
        return std::unexpected(invalid("cannot read config file: " + file.string()));
    }
    const auto base = std::filesystem::absolute(file).parent_path();
    std::string line;
    for (int number = 1; std::getline(in, line); ++number) {
        const auto text = trim(std::string_view{line}.substr(0, line.find('#')));
        if (text.empty()) {
            continue;
        }
        const auto where = file.string() + ":" + std::to_string(number) + ": ";
        const auto eq = text.find('=');
        if (eq == std::string_view::npos) {
            return std::unexpected(invalid(where + "expected 'key = value'"));
        }
        if (auto ok = apply_setting(settings, trim(text.substr(0, eq)), trim(text.substr(eq + 1)), base); !ok) {
            return std::unexpected(invalid(where + ok.error().message));
        }
    }
    return {};
}

Result<Settings> parse(std::span<char* const> args, bool& help) {
    Settings settings;
    help = false;
    // O arquivo primeiro, onde quer que --config apareça: as flags valem mais.
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        if (arg == "-h" || arg == "--help") {
            help = true;
            return settings;
        }
        if (arg == "--config" && i + 1 < args.size()) {
            if (auto loaded = load_config(std::filesystem::path{std::string{args[i + 1]}}, settings); !loaded) {
                return std::unexpected(loaded.error());
            }
        }
    }
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        if (!arg.starts_with("--") || arg.size() == 2) {
            return std::unexpected(invalid("unknown argument: " + std::string{arg}));
        }
        if (i + 1 >= args.size()) {
            return std::unexpected(invalid("missing value for " + std::string{arg}));
        }
        const std::string_view value{args[++i]};
        if (arg == "--config") {
            continue;
        }
        std::string key{arg.substr(2)};
        std::ranges::replace(key, '-', '_');
        if (auto ok = apply_setting(settings, key, value); !ok) {
            if (ok.error().message.starts_with("unknown setting")) {
                return std::unexpected(invalid("unknown argument: " + std::string{arg}));
            }
            return std::unexpected(ok.error());
        }
    }
    if (settings.proxy.engine.empty()) {
        return std::unexpected(invalid("--engine is required"));
    }
    return settings;
}

Result<std::shared_ptr<modb::proxy::Policy>> make_policy(std::string_view name) {
    if (name == "passthrough") {
        return std::make_shared<modb::proxy::PassThroughPolicy>();
    }
    return std::unexpected(invalid("unknown policy: " + std::string{name}));
}

constexpr std::string_view k_usage =
    "usage: modb-proxy [--config FILE] --engine SOCKET [options]\n"
    "  --config FILE          'key = value' lines (engine, host, port, ...); flags override it\n"
    "  --engine SOCKET        the engine's local socket (engine started with --local)\n"
    "  --secret-file FILE     link secret shared with the engine\n"
    "  --host HOST            listen address (default 127.0.0.1)\n"
    "  --port N               TCP port for clients (default 7474; 0 = any free port)\n"
    "  --policy NAME          passthrough (default)\n"
    "  --name NAME            proxy name in the engine logs (default modb-proxy)\n"
    "  --idle-timeout-ms N    close idle clients after N ms\n"
    "  --compression rle|none codec offered to clients (default rle)\n"
    "  --stream-credit N      frames in flight per stream (default 8)\n"
    "  --reconnect-max-ms N   longest wait between link reconnection attempts (default 5000)\n";

modb::proxy::Proxy* g_proxy = nullptr;

void on_stop_signal(int) {
    if (g_proxy != nullptr) {
        g_proxy->request_stop();
    }
}

} // namespace

int main(int argc, char** argv) {
    bool help = false;
    auto settings = parse(std::span<char* const>{argv, static_cast<std::size_t>(argc)}, help);
    if (help) {
        std::cout << k_usage;
        return 0;
    }
    if (!settings) {
        std::cerr << "error: " << settings.error().message << '\n' << k_usage;
        return 2;
    }
    auto policy = make_policy(settings->policy);
    if (!policy) {
        std::cerr << "error: " << policy.error().message << '\n';
        return 2;
    }
    auto proxy = modb::proxy::Proxy::start(settings->proxy, *policy);
    if (!proxy) {
        std::cerr << "error: " << proxy.error().message << '\n';
        return 1;
    }
    g_proxy = &*proxy;
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, on_stop_signal);
#endif
    std::cout << "READY " << proxy->port() << '\n';
    std::cout << "proxy " << settings->proxy.name << " on " << settings->proxy.host << ':' << proxy->port()
              << " -> " << settings->proxy.engine.string() << " (policy " << (*policy)->name() << ")" << std::endl;
    const auto served = proxy->serve_forever();
    g_proxy = nullptr;
    if (!served) {
        std::cerr << "error: " << served.error().message << '\n';
        return 1;
    }
    std::cout << "stopped" << std::endl;
    return 0;
}
