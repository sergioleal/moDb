#pragma once

// `series_key` — o que pode ser comparado com o quê (docs/PLANO_TESTES_DE_CARGA.md
// §13.4). Hash estável sobre os atributos que precisam ser idênticos para
// dois pontos pertencerem à mesma série. `environment` (o id cadastrado, §4.4)
// NÃO entra no hash -- só `host_class` entra, de propósito: dois ambientes
// registrados com hardware equivalente permanecem na mesma série.

#include <cstdint>
#include <string>

namespace modb::loadtest {

// Versão 2 (2026-07-26): `page_size` passou a chegar aqui de fato -- a campanha
// gravava o campo como string JSON e o rollup lia com get_number, então TODA
// chave da versão 1 foi calculada com page_size=0. Sem a correção, corridas com
// páginas de 4k/8k/16k colidiriam numa série só, o que quebraria justamente a
// varredura de page size do plano de profiling. `instrumentation` entrou no
// mesmo passo: uma corrida com -pg é 2-3x mais lenta e não pertence à série de
// uma corrida limpa. Os pontos da versão 1 continuam válidos e não são
// reescritos -- a camada histórica é append-only
// (docs/PLANO_TESTES_DE_CARGA.md §13.2, docs-process/PLANO_PROFILING.md §3).
inline constexpr std::uint32_t series_key_version = 2;

struct SeriesKeyInput {
    std::string case_id;
    std::uint32_t workload_version{};
    std::string dataset_id;
    std::uint32_t dataset_version{};

    // Parâmetros semânticos efetivos (§4.5).
    std::string scale;
    std::string payload;
    std::uint64_t batch{};
    std::uint64_t concurrency{};
    std::string durability;
    std::string cache;
    std::string primary_storage;

    // Classe de build, instrumentação, arquitetura, page size, versões de
    // formato/protocolo.
    std::string build_type;
    std::string instrumentation;
    std::string arch;
    std::uint64_t page_size{};
    std::uint64_t format_version{};
    std::uint64_t protocol_version{};

    // host_class (resolvido do ambiente registrado, nunca o environment id
    // em si) e alvo de execução.
    std::string host_class;
    std::string target;
};

// Hash estável (16 hex chars, sha256 truncado) sobre `input`. Determinístico:
// mesma entrada produz sempre a mesma chave.
[[nodiscard]] std::string compute_series_key(const SeriesKeyInput& input);

} // namespace modb::loadtest
