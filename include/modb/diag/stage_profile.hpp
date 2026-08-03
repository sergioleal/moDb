#pragma once

// Atribuição de tempo por estágio do caminho quente (docs-process/
// PLANO_PROFILING.md, Etapa 1).
//
// Por que instrumentação in-process e não um profiler amostral: nesta toolchain
// não existe profiler com símbolos para o binário Windows (MinGW emite DWARF em
// PE; VTune e WPA querem PDB). E mesmo onde existe, um profiler amostral de CPU
// não enxerga espera de I/O nem de `fsync` -- que é justamente metade da
// pergunta em aberto sobre o caminho de escrita.
//
// Custo quando desligado: zero. `ScopedStage` vira uma classe vazia e as
// funções viram no-ops inline, sem tocar em nenhum contador. O motor é compilado
// sem instrumentação por padrão; o preset `stage-profile` liga.
//
// Custo quando ligado: uma leitura de relógio monotônico (QPC no Windows,
// ~20-25 ns) e um fetch_add relaxed por escopo. Os estágios instrumentados são
// escolhidos para ficarem na casa do microssegundo, então a distorção fica na
// casa de 1-2%. Estágios muito mais finos que isso não devem ser instrumentados
// aqui -- meça-os por contagem, não por tempo.
//
// Limiar concreto da regra acima (PLANO_PROFILER.md §7, risco 1): um estágio
// cuja duração típica fique abaixo de ~1 us carrega 10-25% de erro só do
// relógio. Se a medição mostrar um estágio nessa faixa, ele deve virar contador
// (`stage_add_units`) em vez de escopo cronometrado -- o número seria dominado
// pelo instrumento, não pelo trabalho.

#include <array>
#include <cstdint>
#include <string_view>

#if defined(MODB_ENABLE_STAGE_PROFILING)
#include <chrono>
#endif

namespace modb::diag {

// Taxonomia fixa. Nomes estáveis: eles saem no JSONL e entram em série
// histórica, então renomear um estágio é como renomear um `scenario_id`.
// A divisão FOLHA / ENVELOPE abaixo é a invariante que faz `attributed_ns`
// significar algo: nenhuma folha contém outra folha, então somá-las nunca conta
// o mesmo nanossegundo duas vezes. Um estágio que contém outro é um envelope e
// fica fora da soma (ver stage_envelope_mask). Ao acrescentar um estágio novo,
// a pergunta obrigatória é "o que roda dentro dele?" -- e a resposta decide o
// lado da linha.
enum class Stage : std::size_t {
    // ========================= FOLHAS: escrita =========================

    // Laço de candidatas de TableHeap::insert -- o suspeito principal do termo
    // proporcional à contagem de páginas. `units` conta ITERAÇÕES do laço.
    // Folha: só percorre `capacity_index_` em memória, sem I/O.
    heap_candidate_scan,
    // Escrita de uma página do heap no PageFile. `units` conta bytes.
    heap_page_write,
    // TableHeap::persist_root -- a escrita extra da página raiz por operação.
    persist_root,
    // Wal::append_page_image e afins. `units` conta bytes de WAL.
    wal_append,
    // Wal::sync -- o `fsync` de verdade. `units` não é usado.
    wal_sync,
    // Binding::to_field_values -- converter o objeto do usuário em FieldValues
    // no create/update. Contraparte de escrita do `materialize`, e a outra
    // metade de H6 (`to_field_values` sem zero-copy, um std::function por
    // campo). `units` conta campos convertidos.
    object_bind,
    // Codificação do objeto (ObjectCodec::encode_object). `units` conta bytes
    // do payload codificado.
    object_encode,
    // PageFile::apply_transaction -- write-back das páginas sujas ao disco,
    // depois do WAL durável. `units` conta bytes escritos.
    buffer_pool_writeback,
    // PageFile::flush -- FlushFileBuffers/fsync do arquivo de DADOS, distinto
    // do fsync do WAL (wal_sync). Um commit chama isto 3 ou 4 vezes
    // (database.cpp, Database::commit_transaction), e até a execução de
    // PLANO_PROFILER.md esse custo não tinha nome nenhum: era a maior parcela
    // do resíduo de `tx_commit`. `units` não é usado.
    page_file_sync,
    // Wal::open_durable -- o WAL é reaberto a cada commit (stat + open do
    // arquivo), não mantido aberto. `units` não é usado.
    wal_open,

    // ========================= FOLHAS: leitura =========================

    // Página servida de memória dentro de PageFile::read -- acerto do buffer
    // pool ou read-your-writes do buffer de transação. `units` conta bytes
    // copiados.
    //
    // ATENÇÃO (regra do ~1 us acima): uma cópia de 4 KiB fica na casa de
    // 100-300 ns, abaixo do limiar em que o relógio deixa de distorcer. Está
    // instrumentado como tempo mesmo assim porque é o principal candidato a
    // explicar o custo de leitura, e sem ele a fase `read` não tem folha
    // nenhuma para atribuir uma página vinda do cache. Leia `max_ns` junto com
    // a média antes de concluir qualquer coisa sobre este estágio, e trate o
    // total como limite superior -- parte dele é o instrumento.
    buffer_pool_hit,
    // Erro de cache do buffer pool: a leitura de disco de verdade, incluindo o
    // read-ahead. `units` conta bytes lidos do disco. Mutuamente exclusivo com
    // buffer_pool_hit -- uma PageFile::read entra em exatamente um dos dois.
    buffer_pool_miss,
    // decode_object (ObjectCodec). `units` conta bytes do registro.
    object_decode,
    // Binding::materialize e ProjectionPlan::materialize -- o estágio que testa
    // H6 (std::function por campo, to_field_values sem zero-copy). `units`
    // conta campos materializados.
    materialize,

    // =========================== ENVELOPES ============================
    // Contêm folhas. Ficam FORA de attributed_ns e saem num bloco separado do
    // JSONL. Cada um declara aqui o que contém.

    // Tentativa de inserir numa página candidata produzida por
    // heap_candidate_scan. `calls` é o que interessa: se ficar em zero enquanto
    // heap_candidate_scan itera milhares de vezes por operação, a varredura é
    // trabalho puro sem resultado. `units` conta as candidatas que estavam
    // cheias na prática.
    //
    // Envelope: `try_insert` carrega a página (buffer_pool_hit/miss), grava
    // (heap_page_write) e persiste a raiz (persist_root). Era contado como
    // folha até PLANO_PROFILER.md ser executado -- e por isso o `attributed_ns`
    // das fases de create publicado antes disso está inflado.
    heap_candidate_try,
    // IdentityMap::find/find_at -- resolução de ObjectId para RecordId.
    // `units` conta páginas de diretório (IDMP) tocadas.
    // Envelope: lê a página de diretório (buffer_pool_hit/miss).
    identity_lookup,
    // TableHeap::read -- leitura do registro já endereçado. `units` conta
    // bytes do registro.
    // Envelope: carrega a página de dados (buffer_pool_hit/miss).
    heap_record_read,
    // ObjectStore::index_maintain -- manutenção de índices secundários por
    // create/update/delete. `units` conta chaves de índice tocadas.
    // Envelope: percorre e grava nós de BTree (buffer_pool_hit/miss,
    // buffer_pool_writeback).
    index_update,
    // Transaction::commit inteiro.
    // Envelope: contém wal_append, wal_sync e buffer_pool_writeback. Existe
    // porque mixed_oltp commita por operação -- o custo dominante daquele
    // workload precisa de um nome próprio.
    tx_commit,

    count_
};

inline constexpr std::size_t stage_count = static_cast<std::size_t>(Stage::count_);

// Um envelope contém outros estágios, então somá-lo à cobertura contaria o
// mesmo tempo duas vezes e faria `attributed_ns` passar de 100% -- o que
// transformaria `unattributed_ns` em lixo em vez de resultado. Envelopes são
// emitidos num bloco separado do JSONL e excluídos da soma de cobertura.
static_assert(stage_count <= 64, "a máscara de envelopes é um uint64");
inline constexpr std::uint64_t stage_envelope_mask =
    (std::uint64_t{1} << static_cast<std::size_t>(Stage::heap_candidate_try)) |
    (std::uint64_t{1} << static_cast<std::size_t>(Stage::identity_lookup)) |
    (std::uint64_t{1} << static_cast<std::size_t>(Stage::heap_record_read)) |
    (std::uint64_t{1} << static_cast<std::size_t>(Stage::index_update)) |
    (std::uint64_t{1} << static_cast<std::size_t>(Stage::tx_commit));

[[nodiscard]] inline constexpr bool stage_is_envelope(Stage stage) noexcept {
    return ((stage_envelope_mask >> static_cast<std::size_t>(stage)) & std::uint64_t{1}) != 0;
}

[[nodiscard]] std::string_view stage_name(Stage stage) noexcept;

struct StageTotals {
    std::uint64_t elapsed_ns{0};
    std::uint64_t calls{0};
    // Contador livre, com significado próprio por estágio (ver o enum). Sempre
    // 0 quando o estágio não define um -- nunca um número inventado.
    std::uint64_t units{0};
    // Maior duração de um único escopo deste estágio (PLANO_PROFILER.md §3.3,
    // primeiro degrau). Separa "média ruim" de "cauda ruim" ao custo de um
    // fetch_max relaxed -- sem reservoir sampling nem RNG no caminho quente.
    std::uint64_t max_ns{0};
};

using StageSnapshot = std::array<StageTotals, stage_count>;

// Acumula em `into` o que os contadores registraram entre `before` e `after`
// (PLANO_PROFILER.md §3.2: atribuição por classe de operação pela diferença de
// dois snapshots na fronteira do harness, custo zero dentro do motor). Só é
// correto quando nada mais registrou estágios nesse intervalo -- no mixed_oltp
// isso vale porque a operação inteira roda sob o mutex compartilhado.
inline void stage_accumulate_delta(StageSnapshot& into, const StageSnapshot& before,
                                   const StageSnapshot& after) noexcept {
    for (std::size_t i = 0; i < stage_count; ++i) {
        into[i].elapsed_ns += after[i].elapsed_ns - before[i].elapsed_ns;
        into[i].calls += after[i].calls - before[i].calls;
        into[i].units += after[i].units - before[i].units;
        // max_ns não é subtraível. Se a janela elevou o máximo global, o pico é
        // desta classe e vale registrar; se não elevou, esta classe não produziu
        // pico novo e o máximo dela é desconhecido. O resultado é um limite
        // inferior honesto -- nunca um pico inventado.
        if (after[i].max_ns > before[i].max_ns && after[i].max_ns > into[i].max_ns) {
            into[i].max_ns = after[i].max_ns;
        }
    }
}

#if defined(MODB_ENABLE_STAGE_PROFILING)

inline constexpr bool stage_profiling_enabled = true;

// Acumuladores globais em atômicos relaxed, não thread_local: o motor tem um
// único escritor de transação (ADR-011), mas `mixed_oltp` emite operações de
// várias threads, e somar thread_locals exigiria um registro de threads vivas.
// Relaxed basta -- ninguém lê os contadores para sincronizar nada, só para
// somar no fim da fase.
void stage_record(Stage stage, std::uint64_t elapsed_ns, std::uint64_t units) noexcept;
void stage_add_units(Stage stage, std::uint64_t units) noexcept;
void stage_reset() noexcept;
[[nodiscard]] StageSnapshot stage_snapshot() noexcept;

class ScopedStage {
public:
    explicit ScopedStage(Stage stage) noexcept
        : stage_{stage}, start_{std::chrono::steady_clock::now()} {}

    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;

    // `units` é acumulado junto ao fechar o escopo; quem mede iterações ou
    // bytes só sabe o total no fim.
    void add_units(std::uint64_t units) noexcept { units_ += units; }

    ~ScopedStage() {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        stage_record(stage_, static_cast<std::uint64_t>(
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                                     .count()),
                     units_);
    }

private:
    Stage stage_;
    std::chrono::steady_clock::time_point start_;
    std::uint64_t units_{0};
};

#else

inline constexpr bool stage_profiling_enabled = false;

inline void stage_record(Stage, std::uint64_t, std::uint64_t) noexcept {}
inline void stage_add_units(Stage, std::uint64_t) noexcept {}
inline void stage_reset() noexcept {}
[[nodiscard]] inline StageSnapshot stage_snapshot() noexcept { return {}; }

class ScopedStage {
public:
    explicit ScopedStage(Stage) noexcept {}
    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;
    void add_units(std::uint64_t) noexcept {}
};

#endif

} // namespace modb::diag
