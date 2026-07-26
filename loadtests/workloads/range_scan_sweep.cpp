#include "workloads/range_scan_sweep.hpp"

#include "target_embedded.hpp"

namespace modb::loadtest {

CaseRunResult run_range_scan_sweep(const Case& c, const std::filesystem::path& work_dir,
                                   std::uint64_t seed, const ProgressCallback& on_progress,
                                   std::filesystem::path& out_db_path) {
    if (c.target != "embedded") {
        CaseRunResult result;
        result.status = "unimplemented";
        result.error = "range_scan_sweep: alvo '" + c.target +
                       "' ainda não tem target implementado (só 'embedded' nesta subfase)";
        return result;
    }

    WorkloadParams params;
    params.work_dir = work_dir.string();
    params.seed = seed;
    params.object_count = c.objects;
    params.batch = c.batch;
    params.payload = c.payload;
    params.on_progress = on_progress;

    return run_range_scan_sweep_embedded(params, out_db_path);
}

} // namespace modb::loadtest
