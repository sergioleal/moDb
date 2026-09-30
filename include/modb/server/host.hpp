#pragma once

// Servidor de aplicação: o banco como processo, com as regras da aplicação
// compiladas dentro dele como stored procedures (docs-process/PLANO_SERVIDOR_PROCS.md,
// ADR-025).
//
// Uma aplicação descreve seus módulos (tipos + procs) e o `server_host` faz o
// resto: argumentos, abertura do banco, bind dos tipos, manifesto e carga de
// cada módulo (ModuleLoader, ADR-012), sinais e o laço do servidor. O `main` de
// um servidor é gerado pela função CMake `modb_add_server` (cmake/ModbServer.cmake).
//
// As procs rodam no processo do banco, sem sandbox: um erro ou exceção desfaz
// só a transação da chamada (OperationRegistry::dispatch); um crash derruba o
// servidor, e o WAL recupera tudo que foi confirmado na próxima abertura.

#include "modb/error.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"
#include "modb/ops/module_manifest.hpp"
#include "modb/ops/operation_registry.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace modb::server {

// Uma configuração da aplicação declarada por um módulo (ModuleBuilder::setting).
// No `.conf` é `<módulo>.<nome> = valor`; na linha de comando,
// `--<módulo>.<nome> valor` (com '-' ou '_'). O valor é validado na subida.
struct ModuleSetting {
    std::string name{};
    std::string default_value{};
    std::string description{};
    // Recusa um valor inválido; vazio = qualquer texto serve.
    std::function<Result<void>(std::string_view)> validate{};
};

// Um módulo de aplicação compilado junto com o servidor.
struct Module {
    // Identifica o módulo no manifesto e nos logs ("biblioteca").
    std::string id{};
    std::uint32_t version{1};
    // Tipos e índices do módulo. Roda na abertura, antes de servir, sem
    // transação aberta (bind/create_index exigem isso).
    std::function<Result<void>(object::Database&)> prepare{};
    // Registra as procs do módulo.
    std::function<Result<void>(ops::OperationRegistry&)> register_procs{};
    // Procs exportadas (id + modo), conferidas contra o registro pelo ModuleLoader.
    std::vector<ops::ExportedMethod> methods{};
    // Descrição de cada proc (id -> texto), para a ajuda e a descoberta.
    std::map<std::string, std::string> descriptions{};
    // Configurações declaradas e onde entregar os valores (nome -> valor, já
    // validados), antes de servir.
    std::vector<ModuleSetting> settings{};
    std::function<void(std::map<std::string, std::string>)> configure{};
};

struct Options {
    std::filesystem::path database{};
    std::string host{"127.0.0.1"};
    std::uint16_t port{7474};
    // Link dos proxies (ADR-028): socket local em que o engine atende os
    // `modb-proxy`. Com ele, o TCP direto fica desligado, a não ser que
    // `tcp = on`.
    std::filesystem::path local{};
    // TCP direto: vazio = ligado sem `local`, desligado com `local`.
    std::optional<bool> tcp{};
    // Segredo que os proxies mandam no LinkHello (arquivo com o segredo).
    std::filesystem::path secret_file{};
    // Threads que executam as sessões de cada link; 0 = núcleos da máquina.
    std::uint32_t link_workers{0};
    // 0 = padrão do net::Server.
    std::uint16_t max_streams{0};
    std::uint32_t idle_timeout_ms{0};
    // Tempo máximo de uma chamada de proc; 0 = sem limite (S5.4).
    std::uint32_t proc_timeout_ms{0};
    // Log de chamadas e mensagens das procs: vazio = stderr, "off" = nenhum,
    // senão um arquivo (acrescenta ao fim) (S5.2).
    std::string log{};
    // Para limpo quando a entrada padrão fecha (supervisores sem sinais, como no
    // Windows). Desligado por padrão: com a entrada em /dev/null pararia na subida.
    bool stop_on_stdin_eof{false};
    // Quanto tempo uma chave de idempotência vale (ADR-029); 0 = desliga as chaves.
    std::uint32_t idempotency_retention_s{86'400};
    // Configurações dos módulos, "<módulo>.<nome>" -> valor; conferidas contra
    // o que cada módulo declara em `start`.
    std::map<std::string, std::string> module_settings{};
};

// Uma configuração `chave = valor` (as mesmas chaves das flags, com '_' no
// lugar de '-': db, host, port, local, tcp, secret_file, link_workers,
// max_streams, idle_timeout_ms, proc_timeout_ms, log, stop_on_stdin_eof,
// idempotency_retention_s), ou `<módulo>.<nome>` para
// uma configuração de módulo (conferida só em `start`, que conhece os módulos).
// Caminhos relativos em `db` e `log` são relativos a `base` (a pasta do arquivo
// de configuração).
[[nodiscard]] Result<void> apply_setting(Options& options, std::string_view key, std::string_view value,
                                         const std::filesystem::path& base = {});
// Lê um arquivo de configuração: linhas `chave = valor`, `#` comenta (S5.1).
[[nodiscard]] Result<void> load_config(const std::filesystem::path& file, Options& options);

// Lê `[--config ARQUIVO] --db ARQUIVO [--host HOST] [--port N] ...`: primeiro o
// arquivo, depois as flags, que têm precedência. `help` vira true com -h/--help.
[[nodiscard]] Result<Options> parse_options(std::span<char* const> args, bool& help);
[[nodiscard]] std::string usage(std::string_view program, std::span<const Module> modules);

// Os valores das configurações de cada módulo (na ordem de `modules`): o padrão
// declarado, ou o de `options`, validado. Uma chave de módulo desconhecida ou um
// valor recusado é erro.
[[nodiscard]] Result<std::vector<std::map<std::string, std::string>>> resolve_module_settings(
    const Options& options, std::span<const Module> modules);

// Confere as configurações dos módulos, abre (ou cria) o banco, prepara e
// carrega os módulos (mais o módulo de sistema "sys", com `sys.procs` e
// `sys.settings`, S6) e devolve o servidor escutando, pronto para `serve_forever`.
[[nodiscard]] Result<net::Server> start(const Options& options, std::span<const Module> modules);

// `main` completo: argumentos, start, SIGINT/SIGTERM → parada limpa,
// "READY <porta>" em stdout quando está aceitando conexões. Devolve o código
// de saída do processo.
int run(int argc, char** argv, std::span<const Module> modules);

} // namespace modb::server
