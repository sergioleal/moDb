#include "profiles.hpp"

#include <algorithm>

namespace modb::loadtest {
namespace {

const std::vector<std::string> kEscadaBasica = {
    "create_only",
    "create_delete_forward",
    "create_delete_reverse",
    "create_delete_interleaved",
    "crud_full",
};

const std::vector<std::string> kAdicionais = {
    "read_hotspot", "range_scan_sweep",     "mixed_oltp",          "snapshot_hold",
    "blob_lifecycle", "cascade_delete", "oversubscribed_churn",
};

std::vector<Case> make_cases(const std::vector<std::string>& workloads,
                             const std::vector<std::string>& targets,
                             const std::vector<std::string>& scales) {
    std::vector<Case> cases;
    for (const auto& w : workloads) {
        for (const auto& t : targets) {
            for (const auto& s : scales) {
                auto objects = objects_for_scale(s);
                if (!objects) {
                    continue; // defensivo; catálogo interno nunca deveria divergir
                }
                Case c;
                c.workload = w;
                c.target = t;
                c.scale = s;
                c.objects = *objects;
                cases.push_back(std::move(c));
            }
        }
    }
    return cases;
}

std::vector<Case> load_smoke_cases() { return make_cases(kEscadaBasica, {"embedded"}, {"1k"}); }

std::vector<Case> load_local_cases() {
    return make_cases(kEscadaBasica, {"embedded", "loopback"}, {"10k", "100k"});
}

std::vector<Case> load_standard_cases() {
    auto cases = make_cases(kEscadaBasica, {"embedded", "loopback", "remote_colocated"}, {"100k"});
    auto extra = make_cases({"create_only", "crud_full"}, {"embedded"}, {"1M"});
    cases.insert(cases.end(), extra.begin(), extra.end());
    return cases;
}

std::vector<Case> load_heavy_cases() {
    // Pairwise nas dimensões secundárias é responsabilidade da Subfase K
    // (ainda não implementada); por ora o conjunto primário já reflete §6.2.
    return make_cases(kEscadaBasica, {"embedded", "remote_colocated"}, {"250k", "500k", "1M"});
}

std::vector<Case> load_remote_cases() {
    return make_cases({"create_only", "crud_full"}, {"remote_colocated", "remote_client_local"},
                      {"10k", "100k"});
}

std::vector<Case> load_soak_cases() {
    return make_cases({"create_delete_interleaved"}, {"embedded"}, {"500k"});
}

std::vector<Case> load_behavior_cases() {
    return make_cases(kAdicionais, {"embedded"}, {"100k"});
}

} // namespace

const std::vector<std::string>& list_profile_names() {
    static const std::vector<std::string> names = {
        "load-smoke",   "load-local",  "load-standard", "load-heavy",
        "load-remote", "load-soak", "load-behavior", "load-diagnostic",
    };
    return names;
}

std::optional<Profile> find_profile(std::string_view name) {
    if (name == "load-smoke") {
        return Profile{"load-smoke", load_smoke_cases()};
    }
    if (name == "load-local") {
        return Profile{"load-local", load_local_cases()};
    }
    if (name == "load-standard") {
        return Profile{"load-standard", load_standard_cases()};
    }
    if (name == "load-heavy") {
        return Profile{"load-heavy", load_heavy_cases()};
    }
    if (name == "load-remote") {
        return Profile{"load-remote", load_remote_cases()};
    }
    if (name == "load-soak") {
        return Profile{"load-soak", load_soak_cases()};
    }
    if (name == "load-behavior") {
        return Profile{"load-behavior", load_behavior_cases()};
    }
    if (name == "load-diagnostic") {
        return Profile{"load-diagnostic", {}};
    }
    return std::nullopt;
}

} // namespace modb::loadtest
