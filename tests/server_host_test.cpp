// Servidor de aplicação (PLANO_SERVIDOR_PROCS S1): argumentos, carga de
// módulos, procs pela rede e o executável gerado por modb_add_server rodando
// como processo à parte -- inclusive morto à força e reaberto.

#include "examples/server_procs/notas_procs.hpp"
#include "modb/app/server_connection.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/server/host.hpp"

#include "test_support.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// Chama uma proc com argumentos Value e decodifica o resultado.
Result<ops::Value> chamar(app::ServerConnection& conn, std::string_view proc, const ops::Value& args) {
    auto bytes = conn.call(proc, ops::encode(args));
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return ops::decode(*bytes);
}

// Roda um comando e junta stdout+stderr (para o `modb call`).
struct Saida {
    int codigo{-1};
    std::string saida;
};
Saida executar(std::string comando) {
    comando += " 2>&1";
#ifdef _WIN32
    // cmd /c tira as aspas das pontas quando a linha começa com aspas: embrulha.
    comando = "\"" + comando + "\"";
    FILE* pipe = _popen(comando.c_str(), "r");
#else
    FILE* pipe = popen(comando.c_str(), "r");
#endif
    Saida s;
    if (pipe == nullptr) {
        return s;
    }
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe) != nullptr) {
        s.saida += buf;
    }
#ifdef _WIN32
    s.codigo = _pclose(pipe);
#else
    const int status = pclose(pipe);
    s.codigo = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return s;
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
        const char* novas[] = {"srv", "--db", "x.modb", "--proc-timeout-ms", "250", "--idle-timeout-ms", "5000",
                               "--max-streams", "8", "--log", "off"};
        auto todas = server::parse_options(std::span<char* const>{const_cast<char**>(novas), 11}, help);
        suite.check(todas && todas->proc_timeout_ms == 250 && todas->idle_timeout_ms == 5000 &&
                        todas->max_streams == 8 && todas->log == "off",
                    "lê --proc-timeout-ms, --idle-timeout-ms, --max-streams e --log");
        const char* desconhecida[] = {"srv", "--db", "x.modb", "--cor", "azul"};
        auto recusada = server::parse_options(std::span<char* const>{const_cast<char**>(desconhecida), 5}, help);
        suite.check(!recusada && recusada.error().message == "unknown argument: --cor", "flag desconhecida é recusada");
    }

    // --- S5.1: arquivo de configuração ---
    {
        const auto pasta = std::filesystem::temp_directory_path() /
                           ("modb-server-host-cfg-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(pasta);
        const auto cfg = pasta / "notas.conf";
        {
            std::ofstream out{cfg};
            out << "# servidor de notas\n"
                   "db = dados/notas.modb\n"
                   "  host=0.0.0.0  \n"
                   "port = 7500   # comentário no fim\n"
                   "proc_timeout_ms = 1000\n"
                   "log = notas.log\n";
        }
        const std::string cfg_str = cfg.string();
        bool help = false;
        const char* so_arquivo[] = {"srv", "--config", cfg_str.c_str()};
        auto lido = server::parse_options(std::span<char* const>{const_cast<char**>(so_arquivo), 3}, help);
        suite.check(lido && lido->host == "0.0.0.0" && lido->port == 7500 && lido->proc_timeout_ms == 1000,
                    "--config lê chave = valor, com espaços e comentários");
        suite.check(lido && lido->database == pasta / "dados/notas.modb" &&
                        lido->log == (pasta / "notas.log").string(),
                    "caminhos relativos do arquivo são relativos à pasta dele");
        const char* com_flag[] = {"srv", "--port", "7600", "--config", cfg_str.c_str()};
        auto sobreposto = server::parse_options(std::span<char* const>{const_cast<char**>(com_flag), 5}, help);
        suite.check(sobreposto && sobreposto->port == 7600 && sobreposto->host == "0.0.0.0",
                    "as flags valem mais que o arquivo, em qualquer ordem");
        {
            std::ofstream out{cfg};
            out << "db = x.modb\nporta = 1\n";
        }
        auto ruim = server::parse_options(std::span<char* const>{const_cast<char**>(so_arquivo), 3}, help);
        suite.check(!ruim && ruim.error().message.find("notas.conf:2: unknown setting: porta") != std::string::npos,
                    "chave desconhecida é recusada com arquivo e linha");
        const char* sem_arquivo[] = {"srv", "--config", "nao-existe.conf"};
        suite.check(!server::parse_options(std::span<char* const>{const_cast<char**>(sem_arquivo), 3}, help),
                    "arquivo de configuração ausente é erro");
        std::error_code ignored;
        std::filesystem::remove_all(pasta, ignored);
    }

    // --- no mesmo processo: start() + cliente real ---
    {
        const auto db = temp_db("inproc");
        auto srv = server::start(server::Options{.database = db, .host = "127.0.0.1", .port = 0}, modulos);
        suite.check(srv.has_value(), "start abre o banco e carrega o módulo");
        if (srv) {
            std::thread laco{[&] { (void)srv->serve_forever(); }};
            if (auto conn = conectar(srv->port(), db); !conn) {
                suite.check(false, "cliente conecta");
            } else {
                auto criada = chamar(*conn, "notas.criar", ops::Value::object({{"texto", "comprar café"}}));
                suite.check(criada && criada->field("id") && criada->field("id")->id(), "notas.criar devolve {id}");
                if (!criada) {
                    std::cerr << "  notas.criar: " << criada.error().message << '\n';
                }
                if (criada && criada->field("id")) {
                    auto lida = chamar(*conn, "notas.ler", ops::Value::object({{"id", *criada->field("id")}}));
                    suite.check(lida && lida->field("texto") && *lida->field("texto")->text() == "comprar café",
                                "notas.ler devolve {id, texto}");
                }
                auto vazia = chamar(*conn, "notas.criar", ops::Value::object({{"texto", ""}}));
                suite.check(!vazia && vazia.error().code == ErrorCode::invalid_argument,
                            "erro de regra chega ao cliente com o código");
                auto sem_texto = chamar(*conn, "notas.criar", ops::Value::object({}));
                suite.check(!sem_texto && sem_texto.error().message.find("'texto' is required") != std::string::npos,
                            "argumento obrigatório ausente é explicado");
                auto tipo_errado = chamar(*conn, "notas.ler", ops::Value::object({{"id", "abc"}}));
                suite.check(!tipo_errado && tipo_errado.error().message.find("must be an object id") != std::string::npos,
                            "argumento com tipo errado é explicado");
                auto sem_proc = conn->call("nao.existe", {});
                suite.check(!sem_proc && sem_proc.error().code == ErrorCode::operation_not_found,
                            "proc desconhecida é operation_not_found");

                // --- S3/S4: erros de regra, índice, consulta, set, remove, rollback ---
                auto repetida = chamar(*conn, "notas.criar", ops::Value::object({{"texto", "comprar café"}}));
                suite.check(!repetida && repetida.error().code == ErrorCode::conflict,
                            "texto repetido é conflict (pelo índice)");
                auto segunda = chamar(*conn, "notas.criar", ops::Value::object({{"texto", "ligar para a Ana"}}));
                auto terceira = chamar(*conn, "notas.criar", ops::Value::object({{"texto", "comprar pão"}}));
                suite.check(segunda && terceira, "cria mais duas notas");
                auto todas = chamar(*conn, "notas.listar", ops::Value::object({}));
                suite.check(todas && todas->list() && todas->list()->size() == 3, "listar devolve as três");
                auto filtradas = chamar(*conn, "notas.listar", ops::Value::object({{"contem", "comprar"}}));
                suite.check(filtradas && filtradas->list() && filtradas->list()->size() == 2, "listar filtra por {contem}");
                if (segunda && segunda->field("id")) {
                    const ops::Value id = *segunda->field("id");
                    auto editada = chamar(*conn, "notas.editar", ops::Value::object({{"id", id}, {"texto", "ligar para o Bruno"}}));
                    suite.check(editada && *editada->field("texto")->text() == "ligar para o Bruno", "editar troca o texto (set)");
                    auto relida = chamar(*conn, "notas.ler", ops::Value::object({{"id", id}}));
                    suite.check(relida && *relida->field("texto")->text() == "ligar para o Bruno", "a edição persiste");
                    auto conflito = chamar(*conn, "notas.editar", ops::Value::object({{"id", id}, {"texto", "comprar pão"}}));
                    suite.check(!conflito && conflito.error().code == ErrorCode::conflict,
                                "editar para um texto que já existe é conflict");
                    suite.check(chamar(*conn, "notas.apagar", ops::Value::object({{"id", id}})).has_value(), "apagar");
                    auto apagada = chamar(*conn, "notas.ler", ops::Value::object({{"id", id}}));
                    suite.check(!apagada && apagada.error().code == ErrorCode::record_not_found &&
                                    apagada.error().message.find("não existe") != std::string::npos,
                                "nota apagada é not_found, com mensagem do módulo");
                }
                auto excecao = chamar(*conn, "notas.excecao", ops::Value::object({{"texto", "não deve ficar"}}));
                suite.check(!excecao && excecao.error().code == ErrorCode::internal_error,
                            "exceção na proc chega como internal_error");
                auto depois = chamar(*conn, "notas.listar", ops::Value::object({{"contem", "não deve ficar"}}));
                suite.check(depois && depois->list() && depois->list()->empty(),
                            "o que a proc escreveu antes da exceção foi desfeito");
                auto de_novo = chamar(*conn, "notas.criar", ops::Value::object({{"texto", "servidor segue no ar"}}));
                suite.check(de_novo.has_value(), "o servidor segue atendendo depois da exceção");
            }
            srv->request_stop();
            laco.join();
        }
        apagar(db);
    }

    // --- S5.2 log por chamada, S5.4 tempo limite, S5.5 parada ativa, S6.1 sys.procs ---
    {
        const auto db = temp_db("operacao");
        const auto log = std::filesystem::path{db.string() + ".log"};
        auto srv = server::start(server::Options{.database = db,
                                                 .host = "127.0.0.1",
                                                 .port = 0,
                                                 .proc_timeout_ms = 200,
                                                 .log = log.string()},
                                 modulos);
        suite.check(srv.has_value(), "start com tempo limite e log em arquivo");
        if (srv) {
            std::thread laco{[&] { (void)srv->serve_forever(); }};
            auto conn = conectar(srv->port(), db);
            suite.check(conn.has_value(), "cliente conecta");
            if (conn) {
                auto procs = chamar(*conn, "sys.procs", ops::Value::object({}));
                bool tem_criar = false;
                bool tem_sys = false;
                if (procs && procs->list()) {
                    for (const auto& p : *procs->list()) {
                        const auto* nome = p.field("name") ? p.field("name")->text() : nullptr;
                        const auto* modo = p.field("mode") ? p.field("mode")->text() : nullptr;
                        const auto* mod = p.field("module") ? p.field("module")->text() : nullptr;
                        const auto* desc = p.field("description") ? p.field("description")->text() : nullptr;
                        if (nome && modo && mod && desc && *nome == "notas.criar") {
                            tem_criar = *modo == "write" && *mod == "notas" && desc->find("único") != std::string::npos;
                        }
                        tem_sys = tem_sys || (nome && *nome == "sys.procs");
                    }
                }
                suite.check(tem_criar, "sys.procs lista as procs com modo, módulo e descrição");
                suite.check(tem_sys, "sys.procs lista a si mesma");

                auto rapida = chamar(*conn, "notas.lenta", ops::Value::object({{"texto", "rápida"}, {"ms", 0}}));
                suite.check(rapida.has_value(), "proc dentro do prazo termina normalmente");
                const auto antes = std::chrono::steady_clock::now();
                auto lenta = chamar(*conn, "notas.lenta", ops::Value::object({{"texto", "lenta"}, {"ms", 400}}));
                suite.check(!lenta && lenta.error().code == ErrorCode::operation_timeout,
                            "proc que passa do prazo falha com operation_timeout");
                suite.check(std::chrono::steady_clock::now() - antes < std::chrono::seconds(3),
                            "a chamada que estourou o prazo volta logo");
                auto depois = chamar(*conn, "notas.listar", ops::Value::object({{"contem", "lenta"}}));
                suite.check(depois && depois->list() && depois->list()->empty(),
                            "o que a proc escreveu antes do prazo foi desfeito");

                // Cliente conectado e ocioso: request_stop fecha a sessão em vez
                // de esperar o idle timeout (30 s).
                const auto parada = std::chrono::steady_clock::now();
                srv->request_stop();
                laco.join();
                const auto levou = std::chrono::steady_clock::now() - parada;
                suite.check(levou < std::chrono::seconds(5), "parada com cliente conectado é imediata");
                if (levou >= std::chrono::seconds(5)) {
                    std::cerr << "  parada levou "
                              << std::chrono::duration_cast<std::chrono::milliseconds>(levou).count() << " ms\n";
                }
                auto morta = chamar(*conn, "notas.listar", ops::Value::object({}));
                suite.check(!morta, "a sessão do cliente foi encerrada");
            } else {
                srv->request_stop();
                laco.join();
            }
        }
        std::string linhas;
        {
            std::ifstream in{log};
            linhas.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
        }
        suite.check(linhas.find(" call sys.procs read ") != std::string::npos &&
                        linhas.find("ms ok") != std::string::npos,
                    "o log tem uma linha por chamada, com proc, modo, tempo e resultado");
        suite.check(linhas.find("call notas.lenta write ") != std::string::npos &&
                        linhas.find("error " + std::to_string(static_cast<unsigned>(ErrorCode::operation_timeout))) !=
                            std::string::npos,
                    "chamada com erro aparece no log com o código");
        suite.check(linhas.size() > 24 && linhas[4] == '-' && linhas[10] == 'T' && linhas.find("Z call") != std::string::npos,
                    "cada linha começa com o instante UTC");
        srv = std::unexpected(Error{ErrorCode::invalid_argument, "fechado"});  // fecha o banco antes de apagar
        apagar(db);
        std::error_code ignored;
        std::filesystem::remove(log, ignored);
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
                auto criada = chamar(*conn, "notas.criar", ops::Value::object({{"texto", "sobrevive ao crash"}}));
                suite.check(criada && criada->field("id") && criada->field("id")->id(), "cria a nota");
                if (criada && criada->field("id") && criada->field("id")->id()) {
                    id_nota = criada->field("id")->id()->value;
                }
            }
            servidor.matar();  // sem desligamento limpo
        }
        // A segunda subida lê tudo de um arquivo de configuração (S5.1).
        const auto cfg = std::filesystem::path{db.string() + ".conf"};
        {
            std::ofstream out{cfg};
            out << "db = " << db.filename().string() << "\nport = " << porta << "\nlog = off\n";
        }
        {
            Filho servidor;
            suite.check(servidor.iniciar({MODB_NOTAS_SERVER_EXE, "--config", cfg.string()}),
                        "sobe o notas-server de novo, pelo arquivo de configuração, sobre o mesmo banco");
            auto conn = conectar(porta, db);
            suite.check(conn.has_value(), "reconecta depois do crash");
            if (conn && id_nota != 0) {
                auto lida = chamar(*conn, "notas.ler", ops::Value::object({{"id", object::ObjectId{id_nota}}}));
                suite.check(lida && lida->field("texto") && *lida->field("texto")->text() == "sobrevive ao crash",
                            "a nota confirmada sobreviveu à morte do processo (WAL)");
            }
            // `modb call` (S3.3): o CLI chama procs com JSON e imprime JSON.
            {
                const std::string base = std::string{"\""} + MODB_CLI_EXE + "\" call 127.0.0.1 " + std::to_string(porta);
                const auto listar = executar(base + " notas.listar");
                suite.check(listar.codigo == 0 && listar.saida.find("\"texto\":\"sobrevive ao crash\"") != std::string::npos,
                            "modb call notas.listar imprime o resultado em JSON");
                const auto sem_id = executar(base + " notas.ler");
                suite.check(sem_id.codigo != 0 && sem_id.saida.find("argument 'id' is required") != std::string::npos,
                            "modb call com erro de proc sai com código e explica o erro");
                const auto procs = executar(std::string{"\""} + MODB_CLI_EXE + "\" procs 127.0.0.1 " + std::to_string(porta));
                suite.check(procs.codigo == 0 && procs.saida.find("notas.criar") != std::string::npos &&
                                procs.saida.find("sys.procs") != std::string::npos &&
                                procs.saida.find("write") != std::string::npos,
                            "modb procs lista as procs do servidor (S6.2)");
                if (procs.codigo != 0) {
                    std::cerr << procs.saida;
                }
            }
            servidor.matar();
        }
        apagar(db);
        std::error_code ignored;
        std::filesystem::remove(cfg, ignored);
    }

    return suite.finish();
}
