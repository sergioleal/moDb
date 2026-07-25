#include "workloads/create_only.hpp"

#include "target_client.hpp"
#include "target_embedded.hpp"

namespace modb::loadtest {

CaseRunResult run_create_only(const Case& c, const std::filesystem::path& work_dir,
                              std::uint64_t seed, const ProgressCallback& on_progress,
                              std::filesystem::path& out_db_path) {
    WorkloadParams params;
    params.work_dir = work_dir.string();
    params.seed = seed;
    params.object_count = c.objects;
    params.batch = c.batch;
    params.payload = c.payload;
    params.on_progress = on_progress;

    if (c.target == "embedded") {
        return run_create_only_embedded(params, out_db_path);
    }
    if (c.target == "loopback") {
        // Subfase G (versão mínima): `on_progress`/`window_interval` ainda
        // não são honrados por este alvo -- nenhum `progress_window` sai
        // daqui, igual a um caso `embedded` cuja fase nunca fecha uma
        // janela. Documentado, não um silêncio acidental.
        return run_create_only_client(params, out_db_path);
    }

    CaseRunResult result;
    result.status = "unimplemented";
    result.error = "create_only: alvo '" + c.target +
                   "' ainda não tem target implementado (só 'embedded' e 'loopback')";
    return result;
}

} // namespace modb::loadtest
