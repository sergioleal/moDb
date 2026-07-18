#pragma once

// Operadores de stream preguiçosos das Fases 7A/7C. Todos consomem e produzem
// Generator<Result<T>> e avaliam sob demanda: nada roda além do que o consumidor
// puxa. `limit` encerra o upstream ao atingir a contagem (curto-circuito),
// `cancellable` encerra assim que o token é sinalizado, e `project`/`compute`
// transformam elemento a elemento mantendo memória O(1).

// Importa Result e códigos de erro.
#include "modb/error.hpp"
// Importa o gerador base.
#include "modb/query/generator.hpp"

// Disponibiliza a flag atômica de cancelamento.
#include <atomic>
// Disponibiliza std::size_t.
#include <cstddef>
// Disponibiliza posse compartilhada do estado de cancelamento.
#include <memory>
// Disponibiliza std::move.
#include <utility>

namespace modb::query {

// Cancelamento cooperativo: uma flag atômica compartilhada por cópias. Os
// operadores a consultam antes de cada elemento; sinalizá-la faz o fluxo
// terminar limpo (as coroutines em cadeia são destruídas em ordem).
class CancellationToken {
public:
    CancellationToken() : flag_{std::make_shared<std::atomic<bool>>(false)} {}

    void cancel() noexcept { flag_->store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool cancelled() const noexcept {
        return flag_->load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// Scan é a fonte (implementada onde há acesso ao armazenamento); os operadores
// abaixo a envolvem.

// limit: cede no máximo `n` itens e encerra — o upstream, mantido como local
// desta coroutine, é destruído ao final, interrompendo a leitura. `n == 0`
// significa "sem limite".
template <typename T>
Generator<Result<T>> limit(Generator<Result<T>> source, std::size_t n) {
    if (n == 0) {
        for (auto& item : source) {
            co_yield std::move(item);
        }
        co_return;
    }
    std::size_t emitted = 0;
    for (auto& item : source) {
        co_yield std::move(item);
        if (++emitted >= n) {
            co_return;
        }
    }
}

// filter (operador Predicate): cede apenas os itens Ok cujo valor satisfaz o
// predicado. Um erro é cedido e encerra o fluxo — não há valor a testar.
template <typename T, typename Predicate>
Generator<Result<T>> filter(Generator<Result<T>> source, Predicate predicate) {
    for (auto& item : source) {
        if (!item) {
            co_yield std::move(item);
            co_return;
        }
        if (predicate(*item)) {
            co_yield std::move(item);
        }
    }
}

// project (Fase 7C): transforma cada Ok em Out via `projector`, elemento a
// elemento. Erros do upstream ou do projector encerram o fluxo. Streaming e
// memória O(1): só o item corrente vive.
template <typename In, typename Out, typename Projector>
Generator<Result<Out>> project(Generator<Result<In>> source, Projector projector) {
    for (auto& item : source) {
        if (!item) {
            co_yield Result<Out>{std::unexpected(std::move(item).error())};
            co_return;
        }
        co_yield projector(*item);
    }
}

// compute (Fase 7C): mesmo pipeline streaming que `project` — nome semântico
// para funções computadas/registradas aplicadas elemento a elemento.
template <typename In, typename Out, typename Function>
Generator<Result<Out>> compute(Generator<Result<In>> source, Function function) {
    for (auto& item : source) {
        if (!item) {
            co_yield Result<Out>{std::unexpected(std::move(item).error())};
            co_return;
        }
        co_yield function(*item);
    }
}

// cancellable: encerra o fluxo assim que o token é sinalizado, checando antes de
// ceder cada elemento.
template <typename T>
Generator<Result<T>> cancellable(Generator<Result<T>> source, CancellationToken token) {
    for (auto& item : source) {
        if (token.cancelled()) {
            co_return;
        }
        co_yield std::move(item);
    }
}

} // namespace modb::query
