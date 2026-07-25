#include "matrix.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace modb::loadtest {
namespace {

// Ids fora desta lista existem no plano mas ainda não têm workload
// implementado (§4.2.1); `is_workload_implemented` é a única fonte de
// verdade sobre o que `run` de fato consegue executar.
const std::vector<std::string> kImplementedWorkloads = {
    "create_only",
    "create_delete_forward",
    "create_delete_reverse",
    "create_delete_interleaved",
};

bool contains(const std::vector<std::string>& values, std::string_view target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

std::vector<std::string> split_dot(std::string_view s) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto dot = s.find('.', start);
        if (dot == std::string_view::npos) {
            parts.emplace_back(s.substr(start));
            break;
        }
        parts.emplace_back(s.substr(start, dot - start));
        start = dot + 1;
    }
    return parts;
}

std::optional<std::uint64_t> parse_u64(std::string_view s) {
    std::uint64_t value{};
    auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    if (result.ec != std::errc{} || result.ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return value;
}

template <typename Setter>
std::vector<Case> cross_expand(std::vector<Case> in, const std::vector<std::string>& values,
                               Setter setter) {
    if (values.empty()) {
        return in;
    }
    std::vector<Case> out;
    out.reserve(in.size() * values.size());
    for (const auto& c : in) {
        for (const auto& v : values) {
            Case copy = c;
            setter(copy, v);
            out.push_back(std::move(copy));
        }
    }
    return out;
}

} // namespace

const std::vector<ScaleInfo>& known_scales() {
    static const std::vector<ScaleInfo> scales = {
        {"1k", 1'000},     {"10k", 10'000},   {"100k", 100'000},
        {"250k", 250'000}, {"500k", 500'000}, {"1M", 1'000'000},
    };
    return scales;
}

std::optional<std::uint64_t> objects_for_scale(std::string_view scale_id) {
    for (const auto& s : known_scales()) {
        if (s.id == scale_id) {
            return s.objects;
        }
    }
    return std::nullopt;
}

bool is_known_scale(std::string_view scale_id) {
    return objects_for_scale(scale_id).has_value();
}

const std::vector<std::string>& known_workloads() {
    static const std::vector<std::string> workloads = {
        // escada básica (§4.2)
        "create_only",
        "create_delete_forward",
        "create_delete_reverse",
        "create_delete_interleaved",
        "crud_full",
        // adicionais em escopo (§4.2.1)
        "read_hotspot",
        "range_scan_sweep",
        "mixed_oltp",
        "snapshot_hold",
        "blob_lifecycle",
        "cascade_delete",
        "oversubscribed_churn",
        "restart_recovery",
        // fora de qualquer perfil até a infraestrutura existir (§4.2.1)
        "schema_evolution",
        "replica_catchup",
    };
    return workloads;
}

bool is_known_workload(std::string_view workload_id) {
    return contains(known_workloads(), workload_id);
}

bool is_workload_implemented(std::string_view workload_id) {
    return contains(kImplementedWorkloads, workload_id);
}

const std::vector<std::string>& known_targets() {
    static const std::vector<std::string> targets = {
        "embedded",
        "loopback",
        "remote_colocated",
        "remote_client_local",
    };
    return targets;
}

bool is_known_target(std::string_view target_id) {
    return contains(known_targets(), target_id);
}

bool is_target_implemented(std::string_view target_id) {
    return target_id == "embedded";
}

std::string Case::case_id() const {
    std::string id = "load." + workload + "." + target + "." + scale;
    std::string suffix = explicit_variant;
    if (suffix.empty()) {
        std::vector<std::string> parts;
        if (concurrency != 1) {
            parts.push_back("c" + std::to_string(concurrency));
        }
        if (payload != "normal") {
            parts.push_back("payload_" + payload);
        }
        if (batch != 1000) {
            parts.push_back("batch" + std::to_string(batch));
        }
        if (durability != "sync_real") {
            parts.push_back("nosync");
        }
        if (cache == "database_reopen") {
            parts.push_back("reopen");
        } else if (cache == "oversubscribed") {
            parts.push_back("oversubscribed");
        }
        if (primary_storage != "full") {
            parts.push_back("walonly");
        }
        for (const auto& p : parts) {
            if (!suffix.empty()) {
                suffix += "_";
            }
            suffix += p;
        }
    }
    if (!suffix.empty()) {
        id += "." + suffix;
    }
    return id;
}

std::string unimplemented_dimension_reason(const Case& c) {
    // --case com sufixo explícito não é decodificado em dimensões concretas
    // (parse_case_id só guarda o texto em explicit_variant) -- não há como
    // validar o que ele promete, então é recusado direto, não silenciosamente
    // aceito como se fosse inócuo.
    if (!c.explicit_variant.empty()) {
        return "variante explícita em --case ('" + c.explicit_variant +
               "') ainda não é suportada: nenhuma dimensão secundária além de "
               "payload/batch tem dispatch implementado (§4.5, dívida D1) -- descreva "
               "a variação por seletor estruturado, não no sufixo do id";
    }
    if (c.concurrency != 1) {
        return "concurrency=" + std::to_string(c.concurrency) +
               " ainda não tem dispatch implementado (chega na Subfase M) -- " + c.case_id();
    }
    if (c.readers != 0) {
        return "readers=" + std::to_string(c.readers) +
               " ainda não tem dispatch implementado -- " + c.case_id();
    }
    if (c.durability != "sync_real") {
        return "durability='" + c.durability + "' ainda não tem dispatch implementado -- " +
               c.case_id();
    }
    if (c.cache != "warm") {
        return "cache='" + c.cache + "' ainda não tem dispatch implementado -- " + c.case_id();
    }
    if (c.primary_storage != "full") {
        return "primary_storage='" + c.primary_storage +
               "' ainda não tem dispatch implementado -- " + c.case_id();
    }
    return "";
}

bool parse_case_id(std::string_view case_id, Case& out, std::string& error) {
    auto parts = split_dot(case_id);
    if (parts.size() < 4 || parts[0] != "load") {
        error = "formato esperado 'load.<workload>.<target>.<scale>[.<variante>]'";
        return false;
    }
    const std::string& workload = parts[1];
    const std::string& target = parts[2];
    const std::string& scale = parts[3];

    if (!is_known_workload(workload)) {
        error = "workload desconhecido: " + workload;
        return false;
    }
    if (!is_known_target(target)) {
        error = "alvo desconhecido: " + target;
        return false;
    }
    auto objects = objects_for_scale(scale);
    if (!objects) {
        error = "escala desconhecida: " + scale;
        return false;
    }

    out = Case{};
    out.workload = workload;
    out.target = target;
    out.scale = scale;
    out.objects = *objects;
    if (parts.size() > 4) {
        std::ostringstream oss;
        for (std::size_t i = 4; i < parts.size(); ++i) {
            if (i > 4) {
                oss << '.';
            }
            oss << parts[i];
        }
        out.explicit_variant = oss.str();
    }
    return true;
}

ExpandResult expand_matrix(const std::vector<Case>& profile_cases,
                           const MatrixSelectors& selectors) {
    ExpandResult result;
    std::vector<Case> working;

    if (!selectors.case_ids.empty()) {
        for (const auto& id : selectors.case_ids) {
            Case c;
            std::string err;
            if (!parse_case_id(id, c, err)) {
                result.error = "--case '" + id + "': " + err;
                return result;
            }
            working.push_back(std::move(c));
        }
    } else {
        for (const auto& s : selectors.scale) {
            if (!is_known_scale(s)) {
                result.error = "--scale: escala desconhecida: " + s;
                return result;
            }
        }
        for (const auto& w : selectors.workload) {
            if (!is_known_workload(w)) {
                result.error = "--workload: workload desconhecido: " + w;
                return result;
            }
        }
        for (const auto& t : selectors.target) {
            if (!is_known_target(t)) {
                result.error = "--target: alvo desconhecido: " + t;
                return result;
            }
        }

        for (const auto& c : profile_cases) {
            const bool scale_ok = selectors.scale.empty() || contains(selectors.scale, c.scale);
            const bool workload_ok =
                selectors.workload.empty() || contains(selectors.workload, c.workload);
            const bool target_ok = selectors.target.empty() || contains(selectors.target, c.target);
            if (scale_ok && workload_ok && target_ok) {
                working.push_back(c);
            }
        }
    }

    working = cross_expand(std::move(working), selectors.environment,
                           [](Case& c, const std::string& v) { c.environment = v; });
    working = cross_expand(std::move(working), selectors.concurrency,
                           [](Case& c, const std::string& v) {
                               if (auto n = parse_u64(v)) {
                                   c.concurrency = *n;
                               }
                           });
    working = cross_expand(std::move(working), selectors.payload,
                           [](Case& c, const std::string& v) { c.payload = v; });

    if (selectors.repeat > 1) {
        std::vector<Case> repeated;
        repeated.reserve(working.size() * selectors.repeat);
        for (const auto& c : working) {
            for (std::uint64_t i = 0; i < selectors.repeat; ++i) {
                Case copy = c;
                copy.repeat_index = i;
                repeated.push_back(std::move(copy));
            }
        }
        working = std::move(repeated);
    }

    if (!selectors.filter.empty()) {
        std::vector<Case> filtered;
        for (auto& c : working) {
            if (c.case_id().find(selectors.filter) != std::string::npos) {
                filtered.push_back(std::move(c));
            }
        }
        working = std::move(filtered);
    }

    for (const auto& ex : selectors.exclude) {
        std::vector<Case> filtered;
        for (auto& c : working) {
            if (c.case_id().find(ex) == std::string::npos) {
                filtered.push_back(std::move(c));
            }
        }
        working = std::move(filtered);
    }

    if (working.empty()) {
        result.error = "conjunto vazio depois da composição de seletores (§6.1) -- nunca "
                       "\"executou zero casos com sucesso\": revise perfil, seletores, filter e exclude";
        return result;
    }

    // Dívida D1 (docs-process/PLANO_IMPLEMENTACAO_CARGA.md §2): recusar aqui,
    // não deixar o case_id prometer uma dimensão que o runtime ignora.
    for (const auto& c : working) {
        auto reason = unimplemented_dimension_reason(c);
        if (!reason.empty()) {
            result.error = reason;
            return result;
        }
    }

    result.cases = std::move(working);
    return result;
}

} // namespace modb::loadtest
