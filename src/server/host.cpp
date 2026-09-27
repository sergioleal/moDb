#include "modb/server/host.hpp"

#include <charconv>
#include <csignal>
#include <iostream>
#include <memory>

namespace modb::server {

namespace {

Error invalid(std::string message) { return Error{ErrorCode::invalid_argument, std::move(message)}; }

// Mensagens das procs (ExecutionContext::logger) vão para stderr, com o módulo.
class StderrLogger final : public ops::Logger {
public:
    void info(std::string_view m) override { std::cerr << "info: " << m << '\n'; }
    void warn(std::string_view m) override { std::cerr << "warn: " << m << '\n'; }
    void error(std::string_view m) override { std::cerr << "error: " << m << '\n'; }
};

StderrLogger g_logger;
net::Server* g_signal_server = nullptr;

void on_stop_signal(int) {
    if (g_signal_server != nullptr) {
        g_signal_server->request_stop();
    }
}

} // namespace

Result<Options> parse_options(std::span<char* const> args, bool& help) {
    Options options;
    help = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        auto value = [&]() -> Result<std::string_view> {
            if (i + 1 >= args.size()) {
                return std::unexpected(invalid("missing value for " + std::string{arg}));
            }
            return std::string_view{args[++i]};
        };
        if (arg == "-h" || arg == "--help") {
            help = true;
            return options;
        }
        if (arg == "--db") {
            auto v = value();
            if (!v) {
                return std::unexpected(v.error());
            }
            options.database = std::filesystem::path{std::string{*v}};
        } else if (arg == "--host") {
            auto v = value();
            if (!v) {
                return std::unexpected(v.error());
            }
            options.host = std::string{*v};
        } else if (arg == "--port") {
            auto v = value();
            if (!v) {
                return std::unexpected(v.error());
            }
            unsigned port = 0;
            const auto [end, ec] = std::from_chars(v->data(), v->data() + v->size(), port);
            if (ec != std::errc{} || end != v->data() + v->size() || port > 65535) {
                return std::unexpected(invalid("invalid port: " + std::string{*v}));
            }
            options.port = static_cast<std::uint16_t>(port);
        } else {
            return std::unexpected(invalid("unknown argument: " + std::string{arg}));
        }
    }
    if (options.database.empty()) {
        return std::unexpected(invalid("--db is required"));
    }
    return options;
}

std::string usage(std::string_view program, std::span<const Module> modules) {
    std::string text = "usage: " + std::string{program} + " --db FILE [--host HOST] [--port N]\n"
                       "  --db    database file (created if it does not exist)\n"
                       "  --host  listen address (default 127.0.0.1)\n"
                       "  --port  TCP port (default 7474; 0 = any free port)\n"
                       "modules:";
    for (const auto& m : modules) {
        text += "\n  " + m.id + " v" + std::to_string(m.version) + ":";
        for (const auto& method : m.methods) {
            text += " " + method.id;
        }
    }
    return text + '\n';
}

Result<net::Server> start(const Options& options, std::span<const Module> modules) {
    auto server = net::Server::listen(options.database, options.host, options.port);
    if (!server) {
        return std::unexpected(server.error());
    }
    auto registry = std::make_shared<ops::OperationRegistry>();
    registry->set_logger(g_logger);
    ops::ModuleLoader loader;
    for (const auto& module : modules) {
        if (module.id.empty() || !module.register_procs) {
            return std::unexpected(invalid("module without id or register_procs"));
        }
        if (module.prepare) {
            if (auto prepared = module.prepare(server->database()); !prepared) {
                return std::unexpected(Error{prepared.error().code,
                                             "module " + module.id + ": " + prepared.error().message});
            }
        }
        // O manifesto é montado aqui, com a baseline que o próprio `prepare`
        // acabou de produzir: o módulo foi compilado com este esquema. A
        // allowlist admite o hash porque o módulo veio no binário do servidor,
        // não de fora (ADR-012 existe para módulos de outra origem).
        const auto& baseline = server->database().current_baseline();
        const object::BaselineId baseline_id = baseline ? baseline->id() : object::BaselineId{};
        ops::ModuleManifest manifest{
            .id = module.id,
            .module_version = module.version,
            .baseline = baseline_id,
            .api_version = ops::runtime_api_version,
            .methods = module.methods,
        };
        manifest.hash = ops::compute_manifest_hash(manifest);
        loader.admit_hash(manifest.hash);
        if (auto loaded = loader.load(manifest, baseline_id, *registry, module.register_procs); !loaded) {
            return std::unexpected(Error{loaded.error().code,
                                         "module " + module.id + ": " + loaded.error().message});
        }
    }
    server->set_operation_registry(std::move(registry));
    return server;
}

int run(int argc, char** argv, std::span<const Module> modules) {
    const std::string program = argc > 0 ? std::filesystem::path{argv[0]}.filename().string() : "server";
    bool help = false;
    auto options = parse_options(std::span<char* const>{argv, static_cast<std::size_t>(argc)}, help);
    if (help) {
        std::cout << usage(program, modules);
        return 0;
    }
    if (!options) {
        std::cerr << "error: " << options.error().message << '\n' << usage(program, modules);
        return 2;
    }
    auto server = start(*options, modules);
    if (!server) {
        std::cerr << "error: " << server.error().message << '\n';
        return 1;
    }
    g_signal_server = &*server;
    std::signal(SIGINT, on_stop_signal);
#ifndef _WIN32
    std::signal(SIGTERM, on_stop_signal);
#endif
    // Linha lida por supervisores e testes para saber que já aceita conexões.
    std::cout << "READY " << server->port() << '\n';
    std::cout << "serving " << std::filesystem::absolute(options->database).string() << " on "
              << options->host << ':' << server->port() << " with " << modules.size() << " module(s)"
              << std::endl;
    const auto served = server->serve_forever();
    g_signal_server = nullptr;
    if (!served) {
        std::cerr << "error: " << served.error().message << '\n';
        return 1;
    }
    std::cout << "stopped" << std::endl;
    return 0;
}

} // namespace modb::server
