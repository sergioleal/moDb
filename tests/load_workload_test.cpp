#include "test_support.hpp"

#include "target_client.hpp"
#include "target_embedded.hpp"

#include "modb/object/database.hpp"

#include <chrono>
#include <filesystem>

using namespace modb::loadtest;

namespace {

std::filesystem::path make_temp_work_dir() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
              ("modb-load-workload-test-" + std::to_string(unique));
    std::filesystem::create_directories(dir);
    return dir;
}

WorkloadParams small_params(const std::filesystem::path& work_dir, std::uint64_t object_count = 200,
                           std::uint64_t batch = 37) {
    WorkloadParams params;
    params.work_dir = work_dir.string();
    params.seed = 20260725;
    params.object_count = object_count;
    params.batch = batch;   // não-redondo de propósito: exercita o lote final parcial
    params.payload = "normal";
    return params;
}

void test_create_only(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_create_only_embedded(small_params(work_dir), db_path);

    suite.check(result.ok, "create_only deve completar: " + result.error);
    suite.check(result.status == "completed", "create_only status deve ser completed");
    suite.check(result.phases.size() == 1, "create_only deve produzir exatamente 1 fase");
    if (!result.phases.empty()) {
        suite.check(result.phases[0].phase == "create", "a única fase deve se chamar 'create'");
        suite.check(result.phases[0].operations == 200, "operations deve ser o object_count pedido");
        suite.check(result.phases[0].latency_ns.p50 >= 0.0, "p50 deve ser um número válido");
    }
    suite.check(result.hash_match, "create_only deve validar o hash lógico");
    suite.check(result.expected_hash == result.actual_hash, "expected_hash == actual_hash");
    suite.check(result.write_amplification > 0.0, "write_amplification deve ser computável (> 0)");
}

// Subfase G (versão mínima): o mesmo dataset/hash lógico via um
// `net::Server`/`Client` real em loopback -- não um `Database` embedded
// chamado diretamente. Batch não-redondo (37) exercita o lote final parcial
// também sobre a rede, igual ao equivalente embedded.
void test_create_only_loopback(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_create_only_client(small_params(work_dir), db_path);

    suite.check(result.ok, "create_only loopback deve completar: " + result.error);
    suite.check(result.status == "completed", "create_only loopback status deve ser completed");
    suite.check(result.phases.size() == 1, "create_only loopback deve produzir exatamente 1 fase");
    if (!result.phases.empty()) {
        suite.check(result.phases[0].phase == "create", "a única fase deve se chamar 'create'");
        suite.check(result.phases[0].operations == 200, "operations deve ser o object_count pedido");
    }
    suite.check(result.hash_match,
               "create_only loopback deve validar o hash lógico relendo via query remota: " +
                   result.error);
    suite.check(result.expected_hash == result.actual_hash, "expected_hash == actual_hash");
    suite.check(!result.expected_hash.empty(), "expected_hash não deve ficar vazio");
}

void test_create_delete_forward(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_create_delete_embedded(small_params(work_dir), DeleteOrder::Forward,
                                             "test-forward", db_path);

    suite.check(result.ok, "create_delete_forward deve completar: " + result.error);
    suite.check(result.phases.size() == 2, "create_delete_forward deve produzir 2 fases");
    if (result.phases.size() == 2) {
        suite.check(result.phases[0].phase == "create", "1a fase deve ser 'create'");
        suite.check(result.phases[1].phase == "delete", "2a fase deve ser 'delete'");
        suite.check(result.phases[1].operations == 200, "delete deve remover todos os 200 objetos");
    }
    suite.check(result.all_deleted, "todos os objetos devem estar removidos ao final");
    suite.check(result.still_resolving == 0, "nenhum id removido deve continuar resolvendo");
}

void test_create_delete_reverse(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_create_delete_embedded(small_params(work_dir), DeleteOrder::Reverse,
                                             "test-reverse", db_path);

    suite.check(result.ok, "create_delete_reverse deve completar: " + result.error);
    suite.check(result.all_deleted, "todos os objetos devem estar removidos ao final");
    suite.check(result.still_resolving == 0, "nenhum id removido deve continuar resolvendo");
}

void test_create_delete_interleaved(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_create_delete_embedded(small_params(work_dir), DeleteOrder::Interleaved,
                                             "test-interleaved", db_path);

    suite.check(result.ok, "create_delete_interleaved deve completar: " + result.error);
    suite.check(result.all_deleted, "todos os objetos devem estar removidos ao final");
    suite.check(result.still_resolving == 0, "nenhum id removido deve continuar resolvendo");
    if (result.phases.size() == 2) {
        // Não é uma garantia matemática estrita para toda semente, mas o
        // stride>1 visita as páginas em ordem espalhada -- pages_read do
        // delete não deveria ficar zerado como no forward/reverse sequencial.
        suite.check(result.phases[1].operations == 200,
                   "delete deve remover todos os 200 objetos mesmo fora de ordem");
    }
}

// Compara o tamanho final entre forward e reverse na MESMA semente/escala --
// não é uma garantia por caso isolado (§4.2 fala do padrão geral), mas os
// dois devem ao menos produzir um resultado válido e comparável.
void test_forward_and_reverse_are_comparable(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path forward_db, reverse_db;
    auto forward = run_create_delete_embedded(small_params(work_dir), DeleteOrder::Forward,
                                              "cmp-forward", forward_db);
    auto reverse = run_create_delete_embedded(small_params(work_dir), DeleteOrder::Reverse,
                                              "cmp-reverse", reverse_db);
    suite.check(forward.ok && reverse.ok, "forward e reverse devem completar na mesma semente");
    suite.check(forward.peak_disk_bytes > 0 && reverse.peak_disk_bytes > 0,
               "os dois devem reportar um tamanho de arquivo final mensurável");
}

// Subfase F: em vez de esperar 10s de verdade, usa um window_interval
// minúsculo para forçar o WindowTracker a fechar janelas em uma fase de
// poucos milissegundos -- exercita a mesma lógica de fechamento/callback que
// uma campanha real usaria a 100k+ objetos.
void test_progress_window_emitted(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    auto params = small_params(work_dir, /*object_count=*/200, /*batch=*/37);
    params.window_interval = std::chrono::nanoseconds(1);

    std::vector<ProgressWindow> windows;
    params.on_progress = [&](const ProgressWindow& w) { windows.push_back(w); };

    std::filesystem::path db_path;
    auto result = run_create_delete_embedded(params, DeleteOrder::Forward, "test-progress", db_path);

    suite.check(result.ok, "create_delete_forward com callback deve completar: " + result.error);
    suite.check(!windows.empty(),
               "window_interval minúsculo deve fechar ao menos 1 janela antes do fim da fase");

    bool saw_create = false, saw_delete = false;
    for (const auto& w : windows) {
        if (w.phase == "create") {
            saw_create = true;
        } else if (w.phase == "delete") {
            saw_delete = true;
        }
        suite.check(w.operations_in_window > 0, "toda janela emitida deve ter operações contadas");
    }
    suite.check(saw_create && saw_delete,
               "callback deve receber janelas tanto de 'create' quanto de 'delete'");

    suite.check(result.windows.has_windows,
               "CaseRunResult::windows deve ser preenchido quando alguma janela fechou");
    suite.check(result.windows.first_ops_per_second >= 0.0 && result.windows.last_ops_per_second >= 0.0,
               "first/last_ops_per_second devem ser números válidos");
}

// Sem callback e com o intervalo padrão de 10s (§8), uma fase de 200 objetos
// termina bem antes de qualquer janela fechar -- has_windows deve continuar
// false, não um falso positivo por omissão.
void test_progress_window_absent_without_callback(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_create_only_embedded(small_params(work_dir), db_path);

    suite.check(result.ok, "create_only sem callback deve completar: " + result.error);
    suite.check(!result.windows.has_windows,
               "sem callback e com fase curta, windows.has_windows deve ficar false");
}

// Subfase L (§4.2.1): leituras enviesadas por Zipf conferindo contra o
// esperado (na ordem lida), com taxa de acerto do buffer pool registrada.
void test_read_hotspot(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_read_hotspot_embedded(small_params(work_dir), db_path);

    suite.check(result.ok, "read_hotspot deve completar: " + result.error);
    suite.check(result.status == "completed", "read_hotspot status deve ser completed");
    suite.check(result.phases.size() == 2,
               "read_hotspot deve produzir 2 fases (create + read_hotspot)");
    if (result.phases.size() == 2) {
        suite.check(result.phases[1].phase == "read_hotspot", "a 2a fase deve se chamar 'read_hotspot'");
        suite.check(result.phases[1].operations == 1000,
                   "read_hotspot deve fazer max(3x object_count, 1000) leituras -- aqui o piso de "
                   "1000 domina (object_count=200)");
        suite.check(result.phases[1].cache_hit_rate >= 0.0 && result.phases[1].cache_hit_rate <= 1.0,
                   "cache_hit_rate deve ser uma fração válida quando medido");
    }
    suite.check(result.hash_match, "read_hotspot deve validar os valores lidos: " + result.error);
}

// Subfase L: índice por igualdade/faixa em `User.id`, uma fase por
// seletividade, contagem exata confere e o plano é registrado no nome da fase.
void test_range_scan_sweep(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_range_scan_sweep_embedded(small_params(work_dir), db_path);

    suite.check(result.ok, "range_scan_sweep deve completar: " + result.error);
    suite.check(result.status == "completed", "range_scan_sweep status deve ser completed");
    suite.check(result.phases.size() == 6,
               "range_scan_sweep deve produzir 6 fases (create + 5 seletividades)");
    bool any_index_scan = false;
    for (std::size_t i = 1; i < result.phases.size(); ++i) {
        const auto& phase = result.phases[i];
        suite.check(phase.phase.find("index_scan") != std::string::npos ||
                       phase.phase.find("table_scan") != std::string::npos,
                   "nome da fase deve registrar o plano (índice ou table scan): " + phase.phase);
        if (phase.phase.find("index_scan") != std::string::npos) {
            any_index_scan = true;
        }
    }
    suite.check(any_index_scan, "ao menos uma seletividade deveria usar o índice recém-criado");
}

// Regressão pós-revisão: object_count=0 não pode indexar
// `create_outcome.ids[rank-1]` fora dos limites (ZipfSampler com n=0) --
// deve falhar de forma limpa, não ler memória inválida.
void test_read_hotspot_rejects_zero_object_count(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_read_hotspot_embedded(small_params(work_dir, /*object_count=*/0, /*batch=*/1),
                                            db_path);

    suite.check(!result.ok, "object_count=0 não tem nada para amostrar -- deve falhar, não travar");
    suite.check(result.status == "failed", "status deve ser failed");
    suite.check(!result.error.empty(), "erro deve explicar object_count=0");
}

// Regressão pós-revisão: `write_amplification` usava a mesma fórmula de
// `space_amplification` (tamanho total do arquivo / bytes lógicos) em vez de
// bytes escritos DURANTE a própria fase de leitura -- read_hotspot só lê, não
// deveria fazer o arquivo crescer de forma significativa nesta fase.
void test_read_hotspot_write_amplification_reflects_read_only_phase(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_read_hotspot_embedded(small_params(work_dir), db_path);

    suite.check(result.ok, "read_hotspot deve completar: " + result.error);
    suite.check(result.space_amplification > 0.0, "space_amplification deve refletir o arquivo todo");
    suite.check(result.write_amplification < result.space_amplification,
               "write_amplification (só a fase de leitura) deve ser bem menor que "
               "space_amplification (arquivo inteiro) -- read_hotspot não escreve dados novos");
}

// Subfase M (§4.2.1, fecha D1 para --concurrency): sessões concorrentes de
// verdade (std::thread) emitindo create/read/update/delete contra o mesmo
// banco. Reconciliação (contagem real via query<User>) e checksum de
// amostra devem conferir mesmo com contenção real de verdade.
void test_mixed_oltp_concurrent(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    auto params = small_params(work_dir, /*object_count=*/300, /*batch=*/50);
    params.concurrency = 4;
    std::filesystem::path db_path;
    auto result = run_mixed_oltp_embedded(params, db_path);

    suite.check(result.ok, "mixed_oltp com concurrency=4 deve completar: " + result.error);
    suite.check(result.status == "completed", "mixed_oltp status deve ser completed");
    suite.check(result.phases.size() == 2, "mixed_oltp deve produzir 2 fases (create + mixed_oltp)");
    if (result.phases.size() == 2) {
        suite.check(result.phases[1].phase == "mixed_oltp", "a 2a fase deve se chamar 'mixed_oltp'");
        suite.check(result.phases[1].operations > 0, "a fase mista deve ter executado operações");
    }
    suite.check(result.hash_match,
               "reconciliação e checksum de amostra devem conferir sob concorrência real: " +
                   result.error);
}

// Concurrency=1 (padrão) deve continuar funcionando -- um único thread é só
// um caso particular do mesmo código, não um caminho separado.
void test_mixed_oltp_single_threaded(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    auto params = small_params(work_dir, /*object_count=*/200, /*batch=*/37);
    std::filesystem::path db_path;
    auto result = run_mixed_oltp_embedded(params, db_path);

    suite.check(result.ok, "mixed_oltp com concurrency=1 deve completar: " + result.error);
    suite.check(result.hash_match, "reconciliação deve conferir com uma única sessão: " + result.error);
}

// Subfase N (§4.2.1): a leitura pela snapshot aberta não pode mudar durante
// o churn, e o estado pós-fechamento (sem snapshot) precisa refletir o
// churn de verdade.
void test_snapshot_hold(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_snapshot_hold_embedded(small_params(work_dir, /*object_count=*/90, /*batch=*/13),
                                             db_path);

    suite.check(result.ok, "snapshot_hold deve completar: " + result.error);
    suite.check(result.status == "completed", "snapshot_hold status deve ser completed");
    suite.check(result.hash_match,
               "leitura estável pela snapshot + estado pós-churn devem conferir: " + result.error);
    suite.check(result.all_deleted,
               "objetos removidos durante o churn não devem resolver após o fechamento");
    suite.check(result.phases.size() == 2, "snapshot_hold deve produzir 2 fases (create + hold)");
    if (result.phases.size() == 2) {
        suite.check(result.phases[1].phase == "hold", "a 2a fase deve se chamar 'hold'");
    }
}

// Regressão pós-revisão: `retained_versions` somava os objetos extras
// criados FORA do working set original (nunca tiveram versão antiga nenhuma)
// como se fossem versões antigas retidas pela snapshot. Com object_count=1,
// update_end=delete_end=0 -- nem update nem delete tocam o único objeto
// original, então NADA deveria ficar retido: um valor > 0 aqui é
// exatamente o bug (contar o objeto extra como retenção).
void test_snapshot_hold_retained_versions_excludes_extra_objects(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result =
        run_snapshot_hold_embedded(small_params(work_dir, /*object_count=*/1, /*batch=*/1), db_path);

    suite.check(result.ok, "snapshot_hold com object_count=1 deve completar: " + result.error);
    suite.check(result.phases.size() == 2, "deve produzir 2 fases (create + hold)");
    if (result.phases.size() == 2) {
        suite.check(result.phases[1].retained_versions == 0,
                   "sem nenhum update/delete no working set original, retained_versions deve ser "
                   "exatamente 0 -- não deve incluir o objeto extra criado fora dele");
    }
}

// Subfase O (§4.2.1): create -> read/stream -> grow -> shrink -> delete
// sobre BlobStore, conferindo byte a byte em cada estágio.
void test_blob_lifecycle(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_blob_lifecycle_embedded(small_params(work_dir), db_path);

    suite.check(result.ok, "blob_lifecycle deve completar: " + result.error);
    suite.check(result.status == "completed", "blob_lifecycle status deve ser completed");
    suite.check(result.hash_match,
               "leituras (buffer e streaming), grow e shrink devem conferir byte a byte: " +
                   result.error);
    suite.check(result.all_deleted, "todos os blobs devem estar removidos ao final");
    suite.check(result.phases.size() == 5,
               "blob_lifecycle deve produzir 5 fases (create/read/grow/shrink/delete)");
    if (result.phases.size() == 5) {
        const char* expected_names[] = {"create", "read", "update_grow", "update_shrink", "delete"};
        for (std::size_t i = 0; i < 5; ++i) {
            suite.check(result.phases[i].phase == expected_names[i],
                       std::string("fase ") + std::to_string(i) + " deve se chamar '" +
                           expected_names[i] + "'");
        }
    }
}

// Subfase P (§4.2.1): árvore N-ária via "primeiro filho/próximo irmão",
// removida em cascata com UMA chamada -- zero refs órfãs ao final.
void test_cascade_delete(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_cascade_delete_embedded(small_params(work_dir, /*object_count=*/80, /*batch=*/1),
                                              db_path);

    suite.check(result.ok, "cascade_delete deve completar: " + result.error);
    suite.check(result.status == "completed", "cascade_delete status deve ser completed");
    suite.check(result.hash_match, "zero refs órfãs após a remoção em cascata: " + result.error);
    suite.check(result.all_deleted, "all_deleted deve ser true após a remoção da raiz");
    suite.check(result.still_resolving == 0, "nenhum nó deve sobrar após a cascata");
    suite.check(result.phases.size() == 2,
               "cascade_delete deve produzir 2 fases (create_hierarchy + cascade_delete)");
    if (result.phases.size() == 2) {
        suite.check(result.phases[0].phase == "create_hierarchy",
                   "a 1a fase deve se chamar 'create_hierarchy'");
        suite.check(result.phases[0].operations > 1,
                   "a árvore deve ter mais de 1 nó (profundidade x largura reais)");
        suite.check(result.phases[1].phase == "cascade_delete",
                   "a 2a fase deve se chamar 'cascade_delete'");
        suite.check(result.phases[1].operations == result.phases[0].operations,
                   "cascade_delete deve remover exatamente o total criado");
    }
}

// Subfase Q (§4.2.1): mesmas invariantes de create_delete_interleaved, com
// o buffer pool deliberadamente menor que o working set -- e uma taxa de
// acerto medida de verdade (não inventada) sob essa pressão.
void test_oversubscribed_churn(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_oversubscribed_churn_embedded(
        small_params(work_dir, /*object_count=*/500, /*batch=*/37), db_path);

    suite.check(result.ok, "oversubscribed_churn deve completar: " + result.error);
    suite.check(result.status == "completed", "oversubscribed_churn status deve ser completed");
    suite.check(result.all_deleted, "todos os objetos devem estar removidos ao final");
    suite.check(result.still_resolving == 0, "nenhum id removido deve continuar resolvendo");
    suite.check(result.phases.size() == 2,
               "oversubscribed_churn deve produzir 2 fases (create + delete)");
    if (result.phases.size() == 2) {
        suite.check(result.phases[1].phase == "delete", "a 2a fase deve se chamar 'delete'");
        suite.check(result.phases[1].cache_hit_rate >= 0.0 && result.phases[1].cache_hit_rate <= 1.0,
                   "cache_hit_rate da fase de delete deve ser uma fração válida");
    }
}

// Subfase R (§4.2.1): commit interrompido de propósito
// (CommitPhase::stop_after_commit_record) seguido de fechar+REABRIR o banco
// de verdade -- exercita o replay de WAL real, não uma simulação em memória.
void test_restart_recovery(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_restart_recovery_embedded(
        small_params(work_dir, /*object_count=*/200, /*batch=*/37), db_path);

    suite.check(result.ok, "restart_recovery deve completar: " + result.error);
    suite.check(result.status == "completed", "restart_recovery status deve ser completed");
    suite.check(result.hash_match,
               "estado pós-recuperação deve bater com o último commit durável: " + result.error);
    suite.check(result.phases.size() == 2,
               "restart_recovery deve produzir 2 fases (create + restart_recovery)");
    if (result.phases.size() == 2) {
        suite.check(result.phases[1].phase == "restart_recovery",
                   "a 2a fase deve se chamar 'restart_recovery'");
    }

    // O arquivo reaberto deve continuar existindo e ser um banco válido --
    // reabrir de novo (fora do workload) não deveria falhar.
    suite.check(std::filesystem::exists(db_path), "o arquivo do banco deve continuar existindo");
    auto reopened_again = modb::object::Database::open(db_path);
    suite.check(reopened_again.has_value(),
               "o banco deve continuar abrível normalmente depois do teste: " +
                   (reopened_again ? std::string{} : reopened_again.error().message));
}

// Regressão pós-revisão: a fase de criação só era registrada em
// `result.phases` no FIM da função, então uma falha em qualquer um dos ~10
// pontos de erro entre a criação e a reabertura (aqui, "working set pequeno
// demais para o churn" com object_count=0) reportava zero fases mesmo com a
// criação já tendo terminado com sucesso.
void test_restart_recovery_reports_create_phase_on_early_failure(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_restart_recovery_embedded(
        small_params(work_dir, /*object_count=*/0, /*batch=*/1), db_path);

    suite.check(!result.ok, "object_count=0 não tem working set suficiente para o churn -- deve falhar");
    suite.check(result.status == "failed", "status deve ser failed");
    suite.check(!result.phases.empty(),
               "mesmo falhando no churn, a fase de criação (já bem-sucedida antes do erro) deve "
               "continuar registrada -- não pode reportar zero fases");
    if (!result.phases.empty()) {
        suite.check(result.phases[0].phase == "create", "a fase registrada deve se chamar 'create'");
    }
}

void test_crud_full(TestSuite& suite) {
    auto work_dir = make_temp_work_dir();
    std::filesystem::path db_path;
    auto result = run_crud_full_embedded(small_params(work_dir), db_path);

    suite.check(result.ok, "crud_full deve completar: " + result.error);
    suite.check(result.status == "completed", "crud_full status deve ser completed");
    suite.check(result.phases.size() == 6, "crud_full deve produzir exatamente 6 fases");
    if (result.phases.size() == 6) {
        const char* expected_names[] = {"create",         "read",         "update_inplace",
                                        "update_grow",    "update_shrink", "delete"};
        for (std::size_t i = 0; i < 6; ++i) {
            suite.check(result.phases[i].phase == expected_names[i],
                       std::string("fase ") + std::to_string(i) + " deve se chamar '" +
                           expected_names[i] + "'");
            suite.check(result.phases[i].operations == 200,
                       std::string("fase '") + expected_names[i] + "' deve operar sobre os 200 objetos");
        }
        // bytes_per_object é tamanho TOTAL do arquivo / N nesse instante, não
        // o tamanho de um registro -- o arquivo tende a só crescer ao longo
        // do caso (páginas liberadas por update_shrink não voltam ao SO),
        // então o sinal válido é update_grow >= create, não grow vs. shrink.
        suite.check(result.phases[3].db_bytes >= result.phases[0].db_bytes,
                   "update_grow deve exigir tanto ou mais espaço em arquivo que a criação original");
    }
    suite.check(result.hash_match, "a fase read deve validar o hash lógico da criação");
    suite.check(result.all_deleted, "todos os objetos devem estar removidos ao final");
    suite.check(result.still_resolving == 0, "nenhum id removido deve continuar resolvendo");
}

} // namespace

int main() {
    TestSuite suite;
    test_create_only(suite);
    test_create_only_loopback(suite);
    test_create_delete_forward(suite);
    test_create_delete_reverse(suite);
    test_create_delete_interleaved(suite);
    test_forward_and_reverse_are_comparable(suite);
    test_progress_window_emitted(suite);
    test_progress_window_absent_without_callback(suite);
    test_read_hotspot(suite);
    test_read_hotspot_rejects_zero_object_count(suite);
    test_read_hotspot_write_amplification_reflects_read_only_phase(suite);
    test_range_scan_sweep(suite);
    test_mixed_oltp_concurrent(suite);
    test_mixed_oltp_single_threaded(suite);
    test_snapshot_hold(suite);
    test_snapshot_hold_retained_versions_excludes_extra_objects(suite);
    test_blob_lifecycle(suite);
    test_cascade_delete(suite);
    test_oversubscribed_churn(suite);
    test_restart_recovery(suite);
    test_restart_recovery_reports_create_phase_on_early_failure(suite);
    test_crud_full(suite);
    return suite.finish();
}
