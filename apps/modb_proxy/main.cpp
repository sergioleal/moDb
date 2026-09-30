// modb-proxy: acesso remoto a um engine moDb que escuta só localmente
// (ADR-028, PLANO_PROXY X5.5).
//
//   modb-proxy --engine /run/modb/app.sock --port 7474 [--policy passthrough]
//
// Imprime "READY <porta>" quando já aceita clientes; SIGINT/SIGTERM param limpo,
// e também o fim da entrada padrão com --stop-on-stdin-eof on.

#include "modb/net/stdin_eof.hpp"
#include "modb/proxy/policies.hpp"
#include "modb/proxy/proxy.hpp"
#include "modb/proxy/token_policy.hpp"

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
#include <vector>

namespace {

using modb::Error;
using modb::ErrorCode;
using modb::Result;

struct Settings {
    modb::proxy::Options proxy{};
    std::string policy{"passthrough"};
    // Arquivo de tokens (vazio = sem autenticação).
    std::filesystem::path tokens{};
    std::filesystem::path allowlist{};
    // Destino da auditoria: vazio/off = nenhum, "stderr", ou um arquivo.
    std::string audit{};
    std::uint32_t max_calls_per_second{0};
    std::uint32_t max_streams_per_principal{0};
    // Para limpo quando a entrada padrão fecha (supervisores sem sinais).
    bool stop_on_stdin_eof{false};
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
    } else if (key == "tokens") {
        settings.tokens = resolve(base, value);
    } else if (key == "allowlist") {
        settings.allowlist = resolve(base, value);
    } else if (key == "audit") {
        settings.audit = value.empty() || value == "off" || value == "stderr" ? std::string{value}
                                                                             : resolve(base, value).string();
    } else if (key == "max_calls_per_second") {
        return set_number(settings.max_calls_per_second);
    } else if (key == "max_streams_per_principal") {
        return set_number(settings.max_streams_per_principal);
    } else if (key == "stop_on_stdin_eof") {
        if (value != "on" && value != "off") {
            return std::unexpected(invalid("stop_on_stdin_eof must be on or off"));
        }
        settings.stop_on_stdin_eof = value == "on";
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

// Monta a cadeia na ordem da PolicyChain: autenticação, allowlist, só leitura,
// limites (contam só o que vai ao engine) e auditoria.
Result<std::shared_ptr<modb::proxy::Policy>> make_policy(const Settings& settings) {
    using namespace modb::proxy;
    std::vector<std::shared_ptr<Policy>> chain;
    if (!settings.tokens.empty()) {
        auto tokens = TokenStore::load(settings.tokens);
        if (!tokens) {
            return std::unexpected(tokens.error());
        }
        chain.push_back(std::make_shared<TokenPolicy>(std::move(*tokens), std::make_shared<PassThroughPolicy>()));
    }
    if (!settings.allowlist.empty()) {
        auto allowlist = AllowlistPolicy::load(settings.allowlist);
        if (!allowlist) {
            return std::unexpected(allowlist.error());
        }
        chain.push_back(std::move(*allowlist));
    }
    if (settings.policy == "read_only") {
        chain.push_back(std::make_shared<ReadOnlyPolicy>());
    } else if (settings.policy != "passthrough") {
        return std::unexpected(invalid("unknown policy: " + settings.policy + " (passthrough or read_only)"));
    }
    if (settings.max_calls_per_second != 0 || settings.max_streams_per_principal != 0) {
        chain.push_back(
            std::make_shared<RateLimitPolicy>(settings.max_calls_per_second, settings.max_streams_per_principal));
    }
    if (settings.audit == "stderr") {
        chain.push_back(AuditLogPolicy::to_stream(std::cerr));
    } else if (!settings.audit.empty() && settings.audit != "off") {
        static std::ofstream audit_file;
        audit_file.open(settings.audit, std::ios::app);
        if (!audit_file) {
            return std::unexpected(invalid("cannot open audit file: " + settings.audit));
        }
        chain.push_back(AuditLogPolicy::to_stream(audit_file));
    }
    if (chain.empty()) {
        return std::shared_ptr<Policy>{std::make_shared<PassThroughPolicy>()};
    }
    return std::shared_ptr<Policy>{std::make_shared<PolicyChain>(std::move(chain))};
}

// modb-proxy hash-token <token> <principal> [role,role]: a linha do arquivo de tokens.
int hash_token(int argc, char** argv) {
    if (argc < 4 || argc > 5) {
        std::cerr << "usage: modb-proxy hash-token <token> <principal> [role1,role2]\n";
        return 2;
    }
    std::vector<std::string> roles;
    if (argc == 5) {
        std::string_view list{argv[4]};
        while (!list.empty()) {
            const auto comma = list.find(',');
            roles.emplace_back(list.substr(0, comma));
            list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        }
    }
    std::cout << modb::proxy::token_line(argv[2], argv[3], roles) << '\n';
    return 0;
}

constexpr std::string_view k_usage =
    "usage: modb-proxy [--config FILE] --engine SOCKET [options]\n"
    "  --config FILE          'key = value' lines (engine, host, port, ...); flags override it\n"
    "  --engine SOCKET        the engine's local socket (engine started with --local)\n"
    "  --secret-file FILE     link secret shared with the engine\n"
    "  --host HOST            listen address (default 127.0.0.1)\n"
    "  --port N               TCP port for clients (default 7474; 0 = any free port)\n"
    "  --policy NAME          passthrough (default) or read_only (only read procs)\n"
    "  --allowlist FILE       only what the rules allow ('<role|user:NAME|*> <call|query|facade|*> <target>')\n"
    "  --audit FILE|stderr    one line per request (default off)\n"
    "  --max-calls-per-second N      per principal (anonymous: per host); 0 = no limit\n"
    "  --max-streams-per-principal N open streams per principal; 0 = no limit\n"
    "  --tokens FILE          require a token (lines 'sha256:<hex> principal [roles]')\n"
    "  --name NAME            proxy name in the engine logs (default modb-proxy)\n"
    "  --idle-timeout-ms N    close idle clients after N ms\n"
    "  --compression rle|none codec offered to clients (default rle)\n"
    "  --stream-credit N      frames in flight per stream (default 8)\n"
    "  --reconnect-max-ms N   longest wait between link reconnection attempts (default 5000)\n"
    "  --stop-on-stdin-eof on|off  stop cleanly when stdin closes (default off)\n"
    "\n"
    "       modb-proxy hash-token <token> <principal> [role1,role2]\n"
    "  prints the tokens-file line for a token\n";

modb::proxy::Proxy* g_proxy = nullptr;

void on_stop_signal(int) {
    if (g_proxy != nullptr) {
        g_proxy->request_stop();
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view{argv[1]} == "hash-token") {
        return hash_token(argc, argv);
    }
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
    auto policy = make_policy(*settings);
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
    if (settings->stop_on_stdin_eof) {
        modb::net::stop_on_stdin_eof([p = &*proxy] { p->request_stop(); });
    }
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
