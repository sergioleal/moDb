#include "modb/server/host.hpp"

#include "modb/net/stdin_eof.hpp"
#include "modb/server/module.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>

namespace modb::server {

namespace {


// Destino das linhas de log: chamadas (uma por proc executada, S5.2) e
// mensagens das procs (ExecutionContext::logger). Cada linha começa com o
// instante UTC.
class LineLog final : public ops::Logger {
public:
    LineLog() = default;
    explicit LineLog(std::ostream* out) : out_{out} {}

    static Result<std::shared_ptr<LineLog>> open(const std::string& target) {
        if (target.empty()) {
            return std::make_shared<LineLog>(&std::cerr);
        }
        if (target == "off") {
            return std::make_shared<LineLog>(nullptr);
        }
        auto log = std::make_shared<LineLog>();
        log->file_.open(target, std::ios::app);
        if (!log->file_) {
            return std::unexpected(Error{ErrorCode::io_error, "cannot open log file: " + target});
        }
        log->out_ = &log->file_;
        return log;
    }

    void write(std::string_view line) {
        if (out_ == nullptr) {
            return;
        }
        const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
        const std::scoped_lock lock{mu_};
        *out_ << std::format("{:%FT%T}Z ", now) << line << std::endl;
    }

    void info(std::string_view m) override { write("info " + std::string{m}); }
    void warn(std::string_view m) override { write("warn " + std::string{m}); }
    void error(std::string_view m) override { write("error " + std::string{m}); }

private:
    std::mutex mu_;
    std::ofstream file_;
    std::ostream* out_{nullptr};
};

// "call notas.criar write 0.412ms ok" / "call notas.criar write 0.120ms error 22 já existe ..."
std::string call_line(const ops::OperationRegistry::CallRecord& call) {
    const double ms = std::chrono::duration<double, std::milli>(call.duration).count();
    std::string line = std::format("call {} {} {:.3f}ms ", call.id,
                                   !call.mode ? "-" : *call.mode == ops::OperationMode::read_only ? "read" : "write", ms);
    line += call.error == nullptr
                ? std::string{"ok"}
                : std::format("error {} {}", static_cast<unsigned>(call.error->code), call.error->message);
    // Chamada que veio por um proxy com cliente identificado (ADR-028).
    if (call.caller != nullptr && !call.caller->anonymous()) {
        line += " by " + call.caller->principal;
    }
    return line;
}

constexpr std::string_view k_sys_procs_description = "As procs deste servidor: nome, modo, módulo e descrição";
constexpr std::string_view k_sys_settings_description =
    "As configurações dos módulos: módulo, nome, padrão e descrição (sem o valor em uso)";

// Módulo de sistema: descoberta das procs do servidor (S6.1).
Module system_module(std::span<const Module> modules) {
    ops::ValueList procs;
    auto add = [&](const std::string& module, const std::string& name, ops::OperationMode mode,
                   std::string_view description) {
        procs.push_back(ops::Value::object({
            {"name", name},
            {"mode", std::string{mode == ops::OperationMode::read_only ? "read" : "write"}},
            {"module", module},
            {"description", std::string{description}},
        }));
    };
    for (const auto& m : modules) {
        for (const auto& method : m.methods) {
            const auto d = m.descriptions.find(method.id);
            add(m.id, method.id, method.mode, d != m.descriptions.end() ? d->second : std::string{});
        }
    }
    add("sys", "sys.procs", ops::OperationMode::read_only, k_sys_procs_description);
    add("sys", "sys.settings", ops::OperationMode::read_only, k_sys_settings_description);
    auto list = std::make_shared<const ops::Value>(std::move(procs));
    // Só nome, padrão e descrição: o valor em uso fica no servidor, que pode
    // ter configuração que não é de todos os clientes.
    ops::ValueList settings;
    for (const auto& m : modules) {
        for (const auto& s : m.settings) {
            settings.push_back(ops::Value::object({
                {"module", m.id},
                {"name", s.name},
                {"default", s.default_value},
                {"description", s.description},
            }));
        }
    }
    auto setting_list = std::make_shared<const ops::Value>(std::move(settings));
    return ModuleBuilder{"sys"}
        .proc("sys.procs", Mode::read_only, std::string{k_sys_procs_description},
              [list](Context&, const ops::Args&) -> Result<ops::Value> { return *list; })
        .proc("sys.settings", Mode::read_only, std::string{k_sys_settings_description},
              [setting_list](Context&, const ops::Args&) -> Result<ops::Value> { return *setting_list; })
        .build();
}

net::Server* g_signal_server = nullptr;

void on_stop_signal(int) {
    if (g_signal_server != nullptr) {
        g_signal_server->request_stop();
    }
}

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

std::filesystem::path resolve(const std::filesystem::path& base, std::string_view value) {
    std::filesystem::path path{std::string{value}};
    return base.empty() || path.is_absolute() ? path : base / path;
}

std::string_view trim(std::string_view s) {
    constexpr std::string_view blanks = " \t\r";
    const auto first = s.find_first_not_of(blanks);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(blanks) - first + 1);
}

} // namespace

Result<void> apply_setting(Options& options, std::string_view key, std::string_view value,
                           const std::filesystem::path& base) {
    auto set_number = [&](auto& field) -> Result<void> {
        auto n = parse_number<std::remove_reference_t<decltype(field)>>(key, value);
        if (!n) {
            return std::unexpected(n.error());
        }
        field = *n;
        return {};
    };
    if (key == "db") {
        options.database = resolve(base, value);
    } else if (key == "host") {
        options.host = std::string{value};
    } else if (key == "port") {
        return set_number(options.port);
    } else if (key == "local") {
        options.local = resolve(base, value);
    } else if (key == "tcp") {
        if (value != "on" && value != "off") {
            return std::unexpected(invalid("tcp must be on or off"));
        }
        options.tcp = value == "on";
    } else if (key == "secret_file") {
        options.secret_file = resolve(base, value);
    } else if (key == "link_workers") {
        return set_number(options.link_workers);
    } else if (key == "max_streams") {
        return set_number(options.max_streams);
    } else if (key == "idle_timeout_ms") {
        return set_number(options.idle_timeout_ms);
    } else if (key == "proc_timeout_ms") {
        return set_number(options.proc_timeout_ms);
    } else if (key == "log") {
        options.log = value.empty() || value == "off" ? std::string{value} : resolve(base, value).string();
    } else if (key == "stop_on_stdin_eof") {
        if (value != "on" && value != "off") {
            return std::unexpected(invalid("stop_on_stdin_eof must be on or off"));
        }
        options.stop_on_stdin_eof = value == "on";
    } else if (key.find('.') != std::string_view::npos) {
        // Configuração de módulo: conferida em `start`, que conhece os módulos.
        options.module_settings[std::string{key}] = std::string{value};
    } else {
        return std::unexpected(invalid("unknown setting: " + std::string{key}));
    }
    return {};
}

Result<std::vector<std::map<std::string, std::string>>> resolve_module_settings(const Options& options,
                                                                                std::span<const Module> modules) {
    std::vector<std::map<std::string, std::string>> resolved;
    std::map<std::string, const ModuleSetting*> declared;
    for (const auto& m : modules) {
        auto& values = resolved.emplace_back();
        for (const auto& s : m.settings) {
            const auto key = m.id + "." + s.name;
            declared.emplace(key, &s);
            const auto given = options.module_settings.find(key);
            const std::string& value = given != options.module_settings.end() ? given->second : s.default_value;
            if (s.validate) {
                if (auto ok = s.validate(value); !ok) {
                    return std::unexpected(invalid("invalid " + key + ": " + ok.error().message));
                }
            }
            values.emplace(s.name, value);
        }
    }
    for (const auto& [key, value] : options.module_settings) {
        if (!declared.contains(key)) {
            return std::unexpected(invalid("unknown setting: " + key));
        }
    }
    return resolved;
}

Result<void> load_config(const std::filesystem::path& file, Options& options) {
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
        if (auto ok = apply_setting(options, trim(text.substr(0, eq)), trim(text.substr(eq + 1)), base); !ok) {
            return std::unexpected(invalid(where + ok.error().message));
        }
    }
    return {};
}

Result<Options> parse_options(std::span<char* const> args, bool& help) {
    Options options;
    help = false;
    // O arquivo primeiro, onde quer que --config apareça: as flags valem mais.
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        if (arg == "-h" || arg == "--help") {
            help = true;
            return options;
        }
        if (arg == "--config" && i + 1 < args.size()) {
            if (auto loaded = load_config(std::filesystem::path{std::string{args[i + 1]}}, options); !loaded) {
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
        if (auto ok = apply_setting(options, key, value); !ok) {
            if (ok.error().message.starts_with("unknown setting")) {
                return std::unexpected(invalid("unknown argument: " + std::string{arg}));
            }
            return std::unexpected(ok.error());
        }
    }
    if (options.database.empty()) {
        return std::unexpected(invalid("--db is required"));
    }
    return options;
}

std::string usage(std::string_view program, std::span<const Module> modules) {
    std::string text = "usage: " + std::string{program} + " [--config FILE] --db FILE [options]\n"
                       "  --config FILE          'key = value' lines (db, host, port, ...); flags override it\n"
                       "  --db FILE              database file (created if it does not exist)\n"
                       "  --host HOST            listen address (default 127.0.0.1)\n"
                       "  --port N               TCP port (default 7474; 0 = any free port)\n"
                       "  --local SOCKET         serve modb-proxy links on a local socket (ADR-028);\n"
                       "                         direct TCP then stays off unless --tcp on\n"
                       "  --tcp on|off           direct TCP clients (default: on without --local)\n"
                       "  --secret-file FILE     link secret the proxies must present\n"
                       "  --link-workers N       threads per proxy link (default: CPU count)\n"
                       "  --max-streams N        concurrent streams per connection\n"
                       "  --idle-timeout-ms N    close idle connections after N ms\n"
                       "  --proc-timeout-ms N    fail (and roll back) a proc call after N ms; 0 = no limit\n"
                       "  --log FILE|off         call log (default stderr)\n"
                       "  --stop-on-stdin-eof on|off  stop cleanly when stdin closes (default off)\n"
                       "  --MODULE.SETTING V     a module setting (listed below; '-' or '_' in the name)\n"
                       "modules:";
    for (const auto& m : modules) {
        text += "\n  " + m.id + " v" + std::to_string(m.version);
        for (const auto& method : m.methods) {
            text += "\n    " + method.id + (method.mode == ops::OperationMode::read_only ? " (read)" : " (write)");
            if (const auto d = m.descriptions.find(method.id); d != m.descriptions.end() && !d->second.empty()) {
                text += "  " + d->second;
            }
        }
        for (const auto& s : m.settings) {
            text += "\n    setting " + m.id + "." + s.name + " (default '" + s.default_value + "')";
            if (!s.description.empty()) {
                text += "  " + s.description;
            }
        }
    }
    return text + "\n  sys\n    sys.procs (read)  " + std::string{k_sys_procs_description} +
           "\n    sys.settings (read)  " + std::string{k_sys_settings_description} + '\n';
}

Result<net::Server> start(const Options& options, std::span<const Module> modules) {
    // Antes de abrir o banco: um erro de configuração aparece na subida, e não
    // na primeira chamada que a usa.
    auto settings = resolve_module_settings(options, modules);
    if (!settings) {
        return std::unexpected(settings.error());
    }
    for (std::size_t i = 0; i < modules.size(); ++i) {
        if (modules[i].configure) {
            modules[i].configure(std::move((*settings)[i]));
        }
    }
    auto log = LineLog::open(options.log);
    if (!log) {
        return std::unexpected(log.error());
    }
    auto server = net::Server::open(options.database);
    if (!server) {
        return std::unexpected(server.error());
    }
    if (!options.local.empty()) {
        if (!options.secret_file.empty()) {
            std::ifstream in{options.secret_file, std::ios::binary};
            if (!in) {
                return std::unexpected(invalid("cannot read secret file: " + options.secret_file.string()));
            }
            std::string secret{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r' || secret.back() == ' ')) {
                secret.pop_back();
            }
            server->set_link_secret(std::move(secret));
        }
        server->set_link_workers(options.link_workers);
        if (auto listening = server->listen_local(options.local); !listening) {
            return std::unexpected(listening.error());
        }
    }
    if (options.tcp.value_or(options.local.empty())) {
        if (auto listening = server->listen_tcp(options.host, options.port); !listening) {
            return std::unexpected(listening.error());
        }
    }
    if (server->port() == 0 && server->local_path().empty()) {
        return std::unexpected(invalid("nothing to listen on: set --local or turn --tcp on"));
    }
    if (options.max_streams != 0) {
        server->set_max_concurrent_streams(options.max_streams);
    }
    if (options.idle_timeout_ms != 0) {
        server->set_idle_timeout_ms(options.idle_timeout_ms);
    }
    auto registry = std::make_shared<ops::OperationRegistry>();
    registry->set_logger(**log);
    // O observador mantém o log vivo enquanto o registro existir.
    registry->set_call_observer(
        [log = *log](const ops::OperationRegistry::CallRecord& call) { log->write(call_line(call)); });
    registry->set_time_limit(std::chrono::milliseconds{options.proc_timeout_ms});

    std::vector<Module> all{modules.begin(), modules.end()};
    all.push_back(system_module(modules));
    ops::ModuleLoader loader;
    for (const auto& module : all) {
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
    std::signal(SIGTERM, on_stop_signal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, on_stop_signal);  // Ctrl+Break / serviço no Windows
#endif
    if (options->stop_on_stdin_eof) {
        net::stop_on_stdin_eof([s = &*server] { s->request_stop(); });
    }
    // Linha lida por supervisores e testes para saber que já aceita conexões.
    std::cout << "READY " << server->port() << '\n';
    std::cout << "serving " << std::filesystem::absolute(options->database).string();
    if (server->port() != 0) {
        std::cout << " on " << options->host << ':' << server->port();
    }
    if (!server->local_path().empty()) {
        std::cout << " on local socket " << server->local_path().string();
    }
    std::cout << " with " << modules.size() << " module(s)"
              << (options->proc_timeout_ms != 0
                      ? ", proc timeout " + std::to_string(options->proc_timeout_ms) + " ms"
                      : std::string{})
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
