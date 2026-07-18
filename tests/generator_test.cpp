// Valida o Generator de coroutines e os operadores preguiçosos da Fase 7A
// isoladamente (sem armazenamento): preguiça comprovada por contador, composição
// filter/limit, cancelamento cooperativo e destruição precoce sem vazamento
// (relevante sob o preset sanitizers).
#include "modb/query/generator.hpp"
#include "modb/query/operators.hpp"
#include "test_support.hpp"

#include <vector>

using namespace modb;
using namespace modb::query;

namespace {

// Fonte infinita: a cada valor cedido incrementa `*produced`, expondo quantas
// vezes o upstream realmente avançou (prova de preguiça).
Generator<Result<int>> counting(int start, int* produced) {
    for (int value = start;; ++value) {
        ++*produced;
        co_yield Result<int>{value};
    }
}

// Fonte finita a partir de um vetor, para composição determinística.
Generator<Result<int>> from_vector(std::vector<int> values) {
    for (int value : values) {
        co_yield Result<int>{value};
    }
}

std::vector<int> collect(Generator<Result<int>> gen) {
    std::vector<int> out;
    for (auto& item : gen) {
        if (item) {
            out.push_back(*item);
        }
    }
    return out;
}

} // namespace

int main() {
    TestSuite suite;

    // --- preguiça: uma fonte infinita + limit(5) avança o upstream exatamente 5 ---
    {
        int produced = 0;
        auto got = collect(limit(counting(0, &produced), 5));
        suite.check(got == std::vector<int>{0, 1, 2, 3, 4},
                    "limit yields exactly the first n values");
        suite.check(produced == 5,
                    "limit advances an infinite upstream exactly n times (laziness)");
    }

    // --- limit(0) significa sem limite (sobre uma fonte finita) ---
    {
        auto got = collect(limit(from_vector({10, 20, 30}), 0));
        suite.check(got == std::vector<int>{10, 20, 30}, "limit 0 means unbounded");
    }

    // --- composição filter ∘ limit ---
    {
        int produced = 0;
        auto even = [](int value) { return value % 2 == 0; };
        auto got = collect(limit(filter(counting(0, &produced), even), 3));
        suite.check(got == std::vector<int>{0, 2, 4},
                    "filter then limit yields the first three even values");
        // Para achar 3 pares (0,2,4) a fonte precisou produzir 0..4.
        suite.check(produced == 5, "filter pulls the upstream only until the limit is met");
    }

    // --- filter sozinho descarta os que não casam ---
    {
        auto positive = [](int value) { return value > 0; };
        auto got = collect(filter(from_vector({-1, 2, -3, 4, 0, 5}), positive));
        suite.check(got == std::vector<int>{2, 4, 5}, "filter keeps only matching values");
    }

    // --- um erro no fluxo é propagado e encerra ---
    {
        auto source = []() -> Generator<Result<int>> {
            co_yield Result<int>{1};
            co_yield Result<int>{std::unexpected(Error{ErrorCode::io_error, "boom"})};
            co_yield Result<int>{3};
        };
        std::vector<int> oks;
        bool saw_error = false;
        auto always = [](int) { return true; };
        for (auto& item : filter(source(), always)) {
            if (item) {
                oks.push_back(*item);
            } else {
                saw_error = true;
            }
        }
        suite.check(oks == std::vector<int>{1} && saw_error,
                    "an error is forwarded and stops the stream");
    }

    // --- cancelamento cooperativo: parar após 3 elementos ---
    {
        int produced = 0;
        CancellationToken token;
        auto gen = cancellable(counting(0, &produced), token);
        std::vector<int> got;
        int seen = 0;
        for (auto& item : gen) {
            if (item) {
                got.push_back(*item);
            }
            if (++seen == 3) {
                token.cancel();
            }
        }
        suite.check(got == std::vector<int>{0, 1, 2},
                    "a cancelled stream stops after the current element");
    }

    // --- destruição precoce: abandonar o generator no meio não quebra ---
    {
        int produced = 0;
        {
            auto gen = counting(0, &produced);
            int taken = 0;
            for (auto& item : gen) {
                (void)item;
                if (++taken == 4) {
                    break;  // abandona o generator com a coroutine ainda suspensa
                }
            }
            // `gen` sai de escopo aqui: o destrutor destrói a coroutine suspensa.
        }
        suite.check(produced == 4, "abandoning a generator early stops the upstream cleanly");
    }

    return suite.finish();
}
