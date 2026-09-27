// Servidor de aplicação (PLANO_SERVIDOR_PROCS S1): argumentos, carga de
// módulos, procs pela rede e o executável gerado por modb_add_server rodando
// como processo à parte -- inclusive morto à força e reaberto.

#include "examples/server_procs/notas_procs.hpp"
#include "modb/app/server_connection.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/server/host.hpp"

#include "test_support.hpp"

#include <chrono>
#include <iostream>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace modb;
using namespace modb::examples::notas;

namespace {

std::filesystem::path temp_db(std::string_view nome) {
    return std::filesystem::temp_directory_path() /
           ("modb-server-host-" + std::string{nome} + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".modb");
}

void apagar(const std::filesystem::path& db) {
    std::error_code ignored;
    std::filesystem::remove(db, ignored);
    std::filesystem::remove(db.string() + ".wal", ignored);
}

std::uint16_t porta_livre() {
    auto probe = net::NativeSocket::listen("127.0.0.1", 0);
    auto port = probe ? probe->local_port() : Result<std::uint16_t>{std::unexpected(probe.error())};
    return port ? *port : 0;
}

// Tenta conectar até o servidor aceitar (ou o prazo acabar).
Result<app::ServerConnection> conectar(std::uint16_t porta, const std::filesystem::path& db) {
    Error ultimo{ErrorCode::connection_closed, "sem tentativa"};
    for (int i = 0; i < 100; ++i) {
        auto conn = app::ServerConnection::connect(
            app::ConnectionOptions{.host = "127.0.0.1", .port = porta, .database_name = db.filename().string()});
        if (conn) {
            return conn;
        }
        ultimo = conn.error();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return std::unexpected(ultimo);
}

// Processo filho mínimo (Windows / POSIX): sobe, mata à força, espera.
class Filho {
public:
    bool iniciar(const std::vector<std::string>& args) {
#ifdef _WIN32
        std::string linha;
        for (const auto& a : args) {
            linha += (linha.empty() ? "" : " ") + ("\"" + a + "\"");
        }
        STARTUPINFOA si{};
        si.cb = sizeof si;
        std::vector<char> mutavel(linha.begin(), linha.end());
        mutavel.push_back('\0');
        if (!CreateProcessA(nullptr, mutavel.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            nullptr, &si, &pi_)) {
            return false;
        }
        CloseHandle(pi_.hThread);
        vivo_ = true;
        return true;
#else
        pid_ = fork();
        if (pid_ < 0) {
            return false;
        }
        if (pid_ == 0) {
            std::vector<char*> argv;
            for (const auto& a : args) {
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);
            execv(argv[0], argv.data());
            _exit(127);
        }
        vivo_ = true;
        return true;
#endif
    }
    // Morte sem aviso (sem desligamento limpo): o que um crash faria.
    void matar() {
        if (!vivo_) {
            return;
        }
#ifdef _WIN32
        TerminateProcess(pi_.hProcess, 9);
        WaitForSingleObject(pi_.hProcess, 10000);
        CloseHandle(pi_.hProcess);
#else
        kill(pid_, SIGKILL);
        int status = 0;
        waitpid(pid_, &status, 0);
#endif
        vivo_ = false;
    }
    ~Filho() { matar(); }

private:
    bool vivo_{false};
#ifdef _WIN32
    PROCESS_INFORMATION pi_{};
#else
    pid_t pid_{-1};
#endif
};

} // namespace

int main() {
    TestSuite suite;
    const server::Module modulos[] = {modb_module_notas_procs()};

    // --- argumentos ---
    {
        bool help = false;
        const char* sem_db[] = {"srv", "--port", "80"};
        suite.check(!server::parse_options(std::span<char* const>{const_cast<char**>(sem_db), 3}, help),
                    "--db é obrigatório");
        const char* porta_ruim[] = {"srv", "--db", "x.modb", "--port", "70000"};
        suite.check(!server::parse_options(std::span<char* const>{const_cast<char**>(porta_ruim), 5}, help),
                    "porta fora da faixa é recusada");
        const char* ok[] = {"srv", "--db", "x.modb", "--host", "0.0.0.0", "--port", "0"};
        auto lido = server::parse_options(std::span<char* const>{const_cast<char**>(ok), 7}, help);
        suite.check(lido && lido->host == "0.0.0.0" && lido->port == 0 && lido->database == "x.modb",
                    "lê --db, --host e --port");
        const char* ajuda[] = {"srv", "--help"};
        (void)server::parse_options(std::span<char* const>{const_cast<char**>(ajuda), 2}, help);
        suite.check(help, "--help pede a ajuda");
        suite.check(server::usage("srv", modulos).find("notas.criar") != std::string::npos,
                    "a ajuda lista as procs dos módulos");
    }

    // --- no mesmo processo: start() + cliente real ---
    {
        const auto db = temp_db("inproc");
        auto srv = server::start(server::Options{.database = db, .host = "127.0.0.1", .port = 0}, modulos);
        suite.check(srv.has_value(), "start abre o banco e carrega o módulo");
        if (srv) {
            std::thread laco{[&] { (void)srv->serve_forever(); }};
            auto conn = conectar(srv->port(), db);
            suite.check(conn.has_value(), "cliente conecta");
            if (conn) {
                auto id = conn->call(k_criar, texto_para_bytes("comprar café"));
                suite.check(id.has_value(), "notas.criar pela rede");
                if (!id) {
                    std::cerr << "  notas.criar: " << id.error().message << '\n';
                }
                auto lido = id ? conn->call(k_ler, *id) : Result<std::vector<std::byte>>{std::unexpected(id.error())};
                suite.check(lido && bytes_para_texto(*lido) == "comprar café", "notas.ler devolve o texto");
                auto vazia = conn->call(k_criar, {});
                suite.check(!vazia && vazia.error().code == ErrorCode::invalid_argument,
                            "erro de regra chega ao cliente com o código");
                auto sem_proc = conn->call("nao.existe", {});
                suite.check(!sem_proc && sem_proc.error().code == ErrorCode::operation_not_found,
                            "proc desconhecida é operation_not_found");
            }
            srv->request_stop();
            laco.join();
        }
        apagar(db);
    }

    // --- executável gerado por modb_add_server, em outro processo ---
    {
        const auto db = temp_db("processo");
        const auto porta = porta_livre();
        const std::vector<std::string> args{MODB_NOTAS_SERVER_EXE, "--db", db.string(), "--port",
                                            std::to_string(porta)};
        std::uint64_t id_nota = 0;
        {
            Filho servidor;
            suite.check(servidor.iniciar(args), "sobe o notas-server");
            auto conn = conectar(porta, db);
            suite.check(conn.has_value(), "conecta no servidor em outro processo");
            if (conn) {
                auto id = conn->call(k_criar, texto_para_bytes("sobrevive ao crash"));
                auto decodificado = id ? bytes_para_id(*id) : Result<std::uint64_t>{std::unexpected(id.error())};
                suite.check(decodificado.has_value(), "cria a nota");
                id_nota = decodificado.value_or(0);
            }
            servidor.matar();  // sem desligamento limpo
        }
        {
            Filho servidor;
            suite.check(servidor.iniciar(args), "sobe o notas-server de novo sobre o mesmo arquivo");
            auto conn = conectar(porta, db);
            suite.check(conn.has_value(), "reconecta depois do crash");
            if (conn && id_nota != 0) {
                auto lido = conn->call(k_ler, id_para_bytes(id_nota));
                suite.check(lido && bytes_para_texto(*lido) == "sobrevive ao crash",
                            "a nota confirmada sobreviveu à morte do processo (WAL)");
            }
            servidor.matar();
        }
        apagar(db);
    }

    return suite.finish();
}
