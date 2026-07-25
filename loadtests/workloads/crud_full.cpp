#include "workloads/crud_full.hpp"

#include "target_embedded.hpp"

namespace modb::loadtest {

CaseRunResult run_crud_full(const Case& c, const std::filesystem::path& work_dir,
                           std::uint64_t seed, std::filesystem::path& out_db_path) {
    if (c.target != "embedded") {
        CaseRunResult result;
        result.status = "unimplemented";
        result.error = "crud_full: alvo '" + c.target +
                       "' ainda não tem target implementado (só 'embedded' nesta subfase)";
        return result;
    }

    WorkloadParams params;
    params.work_dir = work_dir.string();
    params.seed = seed;
    params.object_count = c.objects;
    params.batch = c.batch;
    params.payload = c.payload;

    return run_crud_full_embedded(params, out_db_path);
}

} // namespace modb::loadtest
