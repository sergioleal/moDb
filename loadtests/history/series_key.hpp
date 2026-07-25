#pragma once

// `series_key` — o que pode ser comparado com o quê (docs/PLANO_TESTES_DE_CARGA.md
// §13.4). Hash estável sobre os atributos que precisam ser idênticos para
// dois pontos pertencerem à mesma série. `environment` (o id cadastrado, §4.4)
// NÃO entra no hash -- só `host_class` entra, de propósito: dois ambientes
// registrados com hardware equivalente permanecem na mesma série.

#include <cstdint>
#include <string>

namespace modb::loadtest {

inline constexpr std::uint32_t series_key_version = 1;

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

    // Classe de build, arquitetura, page size, versões de formato/protocolo.
    std::string build_type;
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
