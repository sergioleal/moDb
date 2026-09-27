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
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace modb::server {

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
};

struct Options {
    std::filesystem::path database{};
    std::string host{"127.0.0.1"};
    std::uint16_t port{7474};
};

// Lê `--db ARQUIVO [--host HOST] [--port N]`. `help` vira true com -h/--help.
[[nodiscard]] Result<Options> parse_options(std::span<char* const> args, bool& help);
[[nodiscard]] std::string usage(std::string_view program, std::span<const Module> modules);

// Abre (ou cria) o banco, prepara e carrega os módulos e devolve o servidor
// escutando, pronto para `serve_forever`.
[[nodiscard]] Result<net::Server> start(const Options& options, std::span<const Module> modules);

// `main` completo: argumentos, start, SIGINT/SIGTERM → parada limpa,
// "READY <porta>" em stdout quando está aceitando conexões. Devolve o código
// de saída do processo.
int run(int argc, char** argv, std::span<const Module> modules);

} // namespace modb::server
