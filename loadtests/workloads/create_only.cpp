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
    if (c.target == "loopback" || c.target == "remote_colocated") {
        // Subfase G (versão mínima): `on_progress`/`window_interval` ainda
        // não são honrados por este alvo -- nenhum `progress_window` sai
        // daqui, igual a um caso `embedded` cuja fase nunca fecha uma
        // janela. Documentado, não um silêncio acidental.
        //
        // Subfase I: `remote_colocated` reaproveita EXATAMENTE o código de
        // `loopback` -- a diferença entre os dois não é de código nenhum,
        // é de ONDE o processo roda (§4.3: "cliente e servidor no mesmo
        // host"). Quando `scripts/run-remote-load.ps1` copia `modb_load`
        // para o host remoto e roda um caso `remote_colocated` lá, o
        // binário nesse host enxerga cliente+servidor locais entre si --
        // exatamente o que `run_create_only_client` já faz.
        return run_create_only_client(params, out_db_path);
    }
    if (c.target == "remote_client_local") {
        // Genuinamente diferente de loopback (cliente precisa alcançar um
        // servidor JÁ rodando num host remoto de verdade, não um que este
        // processo sobe sozinho) -- não implementado nesta subfase por
        // falta de uma segunda máquina disponível para verificar de
        // verdade (ver docs-process/PLANO_IMPLEMENTACAO_CARGA.md, Subfase I).
        CaseRunResult result;
        result.status = "unimplemented";
        result.error =
            "create_only: alvo 'remote_client_local' ainda não tem dispatch implementado -- "
            "exige um servidor já rodando num host remoto (Subfase I, docs-process/"
            "PLANO_IMPLEMENTACAO_CARGA.md)";
        return result;
    }

    CaseRunResult result;
    result.status = "unimplemented";
    result.error = "create_only: alvo '" + c.target +
                   "' ainda não tem target implementado (embedded, loopback, remote_colocated)";
    return result;
}

} // namespace modb::loadtest
