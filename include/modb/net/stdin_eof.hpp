#pragma once

// Parada pelo fim da entrada padrão (PLANO_PROPOSTAS_REGISTRY R3).
//
// No Windows não há como mandar um sinal a um processo filho: `TerminateProcess`,
// o `child.kill()` do Node e o `Stop-Process` encerram à força. Um supervisor
// que abre o processo com um pipe na entrada padrão pede a parada limpa
// fechando o pipe; funciona igual em todos os sistemas.
//
// Só com a opção ligada: um serviço com a entrada ligada a /dev/null (o padrão
// do systemd) receberia o fim da entrada logo na subida e pararia.

#include <cstdio>
#include <functional>
#include <thread>

namespace modb::net {

// Lê a entrada padrão numa thread própria e chama `stop` quando ela fecha. O
// que chegar pela entrada é descartado. A thread fica solta: bloqueada na
// leitura, ela termina com o processo.
inline void stop_on_stdin_eof(std::function<void()> stop) {
    std::thread{[stop = std::move(stop)] {
        char buffer[256];
        while (std::fread(buffer, 1, sizeof buffer, stdin) > 0) {
        }
        stop();
    }}.detach();
}

} // namespace modb::net
