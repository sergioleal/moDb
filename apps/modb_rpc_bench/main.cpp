// modb_rpc_bench: custo de uma chamada de proc por transporte (ADR-026).
//
//   modb_rpc_bench [--calls N] [--warmup N] [--sizes 16,1024,65536] [--transports tcp,shm,proxy]
//                  [--clients 1,8]
//
// Sobe um servidor no próprio processo, com uma proc de leitura `bench.eco`
// que devolve o texto recebido, e mede N chamadas seguidas por cliente (uma de
// cada vez por cliente: latência; com `--clients` > 1, vários clientes ao
// mesmo tempo dão a vazão). `proxy` (ADR-028) põe um `modb::proxy::Proxy`, no
// mesmo processo, na frente do engine, ligado a ele pelo link local: a
// diferença para `tcp` é o custo do salto a mais. Uma linha por combinação, em
// CSV:
//
//   transport,clients,payload_bytes,calls,ops_per_s,p50_us,p99_us,p999_us,max_us
//
// Os números só valem de máquina dedicada (docs-process/PLANO_SERVIDOR_PROCS.md);
// no desktop, servem só para ver que roda.

#include "modb/app/server_connection.hpp"
#include "modb/proxy/proxy.hpp"
#include "modb/server/host.hpp"
#include "modb/server/module.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace modb;

modb::server::Module bench_module() {
    return server::ModuleBuilder{"bench"}
        .proc("bench.eco", server::Mode::read_only, "Devolve {dados}",
              [](server::Context&, const ops::Args& a) -> Result<ops::Value> {
                  auto dados = a.text_or("dados", "");
                  if (!dados) {
                      return std::unexpected(dados.error());
                  }
                  return ops::Value{std::move(*dados)};
              })
        .build();
}

std::vector<std::uint32_t> lista_numeros(std::string_view texto) {
    std::vector<std::uint32_t> out;
    while (!texto.empty()) {
        const auto virgula = texto.find(',');
        const auto parte = texto.substr(0, virgula);
        std::uint32_t v = 0;
        std::from_chars(parte.data(), parte.data() + parte.size(), v);
        out.push_back(v);
        texto = virgula == std::string_view::npos ? std::string_view{} : texto.substr(virgula + 1);
    }
    return out;
}

double percentil(const std::vector<double>& ordenado, double p) {
    const auto i = static_cast<std::size_t>(p * static_cast<double>(ordenado.size() - 1));
    return ordenado[i];
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t calls = 20000;
    std::uint32_t warmup = 2000;
    std::vector<std::uint32_t> sizes{16, 1024, 65536};
    std::vector<std::string> transports{"tcp", "shm", "proxy"};
    std::vector<std::uint32_t> clients_list{1};
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view flag{argv[i]};
        const std::string_view value{argv[i + 1]};
        if (flag == "--calls") {
            calls = lista_numeros(value).at(0);
        } else if (flag == "--warmup") {
            warmup = lista_numeros(value).at(0);
        } else if (flag == "--sizes") {
            sizes = lista_numeros(value);
        } else if (flag == "--clients") {
            clients_list = lista_numeros(value);
        } else if (flag == "--transports") {
            transports.clear();
            std::string_view resto = value;
            while (!resto.empty()) {
                const auto v = resto.find(',');
                transports.emplace_back(resto.substr(0, v));
                resto = v == std::string_view::npos ? std::string_view{} : resto.substr(v + 1);
            }
        } else {
            std::cerr << "usage: modb_rpc_bench [--calls N] [--warmup N] [--sizes 16,1024] [--transports tcp,shm,proxy] "
                         "[--clients 1,8]\n";
            return 2;
        }
    }

    const auto db = std::filesystem::temp_directory_path() /
                    ("modb-rpc-bench-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     ".modb");
    const server::Module modules[] = {bench_module()};
    // O engine atende TCP direto e o link local do proxy ao mesmo tempo.
    const auto sock = std::filesystem::current_path() /
                      ("modb-rpc-bench-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                       ".sock");
    const bool com_proxy = std::ranges::find(transports, std::string{"proxy"}) != transports.end();
    auto srv = server::start(server::Options{.database = db,
                                             .host = "127.0.0.1",
                                             .port = 0,
                                             .local = com_proxy ? sock : std::filesystem::path{},
                                             .tcp = true,
                                             .log = "off"},
                             modules);
    if (!srv) {
        std::cerr << "error: " << srv.error().message << '\n';
        return 1;
    }
    std::thread laco{[&] { (void)srv->serve_forever(); }};

    std::optional<proxy::Proxy> px;
    std::thread laco_proxy;
    if (com_proxy) {
        auto started = proxy::Proxy::start(proxy::Options{.engine = sock, .name = "bench", .port = 0},
                                           std::make_shared<proxy::PassThroughPolicy>());
        if (!started) {
            std::cerr << "error: " << started.error().message << '\n';
            srv->request_stop();
            laco.join();
            return 1;
        }
        px.emplace(std::move(*started));
        laco_proxy = std::thread{[&] { (void)px->serve_forever(); }};
    }

    int status = 0;
    std::cout << "transport,clients,payload_bytes,calls,ops_per_s,p50_us,p99_us,p999_us,max_us\n";
    for (const auto& transport : transports) {
        const auto porta = transport == "proxy" ? px->port() : srv->port();
        for (const auto clientes : clients_list) {
            const auto n = std::max<std::uint32_t>(1, clientes);
            std::vector<app::ServerConnection> conns;
            for (std::uint32_t c = 0; c < n; ++c) {
                auto conn = app::ServerConnection::connect(
                    app::ConnectionOptions{.host = "127.0.0.1", .port = porta, .database_name = "bench"});
                if (!conn) {
                    std::cerr << "error: " << conn.error().message << '\n';
                    status = 1;
                    break;
                }
                if (transport == "shm") {
                    // Anel com folga para o maior payload.
                    const auto maior = *std::ranges::max_element(sizes);
                    if (auto ok = conn->attach_shared_memory(std::max<std::uint32_t>(1u << 20, maior * 2 + 4096)); !ok) {
                        std::cerr << "error: " << ok.error().message << '\n';
                        status = 1;
                        break;
                    }
                }
                conns.push_back(std::move(*conn));
            }
            if (conns.size() != n) {
                continue;
            }
            for (const auto size : sizes) {
                const auto args = ops::encode(ops::Value::object({{"dados", std::string(size, 'x')}}));
                std::vector<std::vector<double>> por_cliente(n);
                std::vector<char> falhou(n, 0);
                const auto medir = [&](std::uint32_t c) {
                    auto& us = por_cliente[c];
                    us.reserve(calls);
                    for (std::uint32_t i = 0; i < warmup + calls; ++i) {
                        const auto t0 = std::chrono::steady_clock::now();
                        auto r = conns[c].call("bench.eco", args);
                        const auto t1 = std::chrono::steady_clock::now();
                        if (!r) {
                            std::cerr << "error: " << r.error().message << '\n';
                            falhou[c] = 1;
                            return;
                        }
                        if (i >= warmup) {
                            us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
                        }
                    }
                };
                const auto inicio = std::chrono::steady_clock::now();
                std::vector<std::thread> threads;
                for (std::uint32_t c = 1; c < n; ++c) {
                    threads.emplace_back(medir, c);
                }
                medir(0);
                for (auto& t : threads) {
                    t.join();
                }
                const auto parede = std::chrono::duration<double>(std::chrono::steady_clock::now() - inicio).count();
                std::vector<double> us;
                for (auto& v : por_cliente) {
                    us.insert(us.end(), v.begin(), v.end());
                }
                if (std::ranges::find(falhou, 1) != falhou.end() || us.empty()) {
                    status = 1;
                    continue;
                }
                std::ranges::sort(us);
                // Vazão pela parede (inclui o aquecimento de cada cliente): com
                // um cliente, o mesmo que 1/média da latência.
                const double ops = static_cast<double>(n) * (warmup + calls) / parede;
                std::printf("%s,%u,%u,%u,%.0f,%.2f,%.2f,%.2f,%.2f\n", transport.c_str(), n, size, calls, ops,
                            percentil(us, 0.50), percentil(us, 0.99), percentil(us, 0.999), us.back());
                std::fflush(stdout);
            }
        }
    }
    if (px) {
        px->request_stop();
        laco_proxy.join();
        px.reset();
    }
    srv->request_stop();
    laco.join();
    srv = std::unexpected(Error{ErrorCode::invalid_argument, "fechado"});
    std::error_code ignored;
    std::filesystem::remove(db, ignored);
    std::filesystem::remove(db.string() + ".wal", ignored);
    return status;
}
