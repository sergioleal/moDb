#pragma once

// Importa Result e os códigos de erro.
#include "modb/error.hpp"
// Importa os subsistemas que o ObjectStore orquestra.
#include "modb/object/baseline.hpp"
#include "modb/object/catalog_store.hpp"
#include "modb/object/database_root.hpp"
#include "modb/object/identity_map.hpp"
#include "modb/object/index_catalog.hpp"
#include "modb/object/object_codec.hpp"
#include "modb/object/type_definition.hpp"
#include "modb/object/type_registry.hpp"
// Importa PageFile e o TableHeap de dados.
#include "modb/storage/page_file.hpp"
#include "modb/storage/table_heap.hpp"
// Importa o gerador de streaming (Fase 7A).
#include "modb/query/generator.hpp"

// Disponibiliza std::size_t no resultado do GC.
#include <unordered_map>
#include <cstddef>
// Disponibiliza std::function para o callback de scan.
#include <functional>
// Disponibiliza std::reference_wrapper nas buscas de tipo.
#include <functional>
// Disponibiliza a baseline corrente opcional.
#include <optional>
// Disponibiliza uma visão leve nas buscas por nome.
#include <string_view>
// Disponibiliza a lista de ids de tipo.
#include <vector>

namespace modb::object {

class Database;

// Amarra o modelo de objetos ao armazenamento persistente: aloca ObjectIds,
// codifica/decodifica objetos, mantém o mapa de identidade, o catálogo e o
// heap de dados. É o primeiro caminho vertical OO — um objeto criado aqui
// sobrevive a fechar e reabrir o arquivo.
//
// O caller possui o PageFile e deve mantê-lo vivo enquanto o ObjectStore
// existir (mesmo contrato de TableHeap).
class ObjectStore {
public:
    // Cria toda a hierarquia num arquivo novo (DBRT, mapa, heaps).
    [[nodiscard]] static Result<ObjectStore> create(storage::PageFile& file);
    // Reabre a hierarquia e reconstrói o catálogo em memória.
    [[nodiscard]] static Result<ObjectStore> open(storage::PageFile& file);

    // Registra um tipo novo, atribuindo seu TypeDefinitionId a partir do
    // contador persistente, gravando-o no catálogo e atualizando a baseline.
    // Busca um tipo registrado por id ou por nome.
    [[nodiscard]] Result<std::reference_wrapper<const TypeDefinition>> find_type(
        TypeDefinitionId id) const;
    [[nodiscard]] Result<std::reference_wrapper<const TypeDefinition>> find_type(
        std::string_view name) const;
    [[nodiscard]] std::vector<std::reference_wrapper<const TypeDefinition>> type_history(
        std::string_view name) const {
        return registry_.history(name);
    }
    // Busca uma baseline histórica por identidade.
    [[nodiscard]] Result<std::reference_wrapper<const Baseline>> find_baseline(
        BaselineId id) const;
    [[nodiscard]] std::span<const Baseline> baselines() const noexcept { return baselines_; }

    // Cria um objeto do tipo dado, validando o payload, persistindo-o e
    // registrando sua identidade. Devolve o ObjectId atribuído.
    // Recupera a versão "current" de um objeto vivo pelo id (sem snapshot).
    [[nodiscard]] Result<DecodedObject> get(ObjectId id);
    // Lê só o TypeDefinitionId do registro (sem decodificar campos) — Fase 10C.
    [[nodiscard]] Result<TypeDefinitionId> peek_type(ObjectId id);
    // Recupera o objeto tal como era visível numa época de snapshot (Fase
    // 6B): current se já valia nessa época, senão previous, senão ausente.
    [[nodiscard]] Result<DecodedObject> get_at(ObjectId id, std::uint64_t snapshot_epoch);
    // Substitui o conteúdo de um objeto, preservando sua identidade.
    // Remove um objeto (o id nunca é reutilizado).
    // Visita cada objeto de dados vivo, na ordem física (versão "current").
    [[nodiscard]] Result<void> scan(
        const std::function<Result<void>(const DecodedObject&)>& visitor);
    // Visita exatamente os objetos visíveis numa época de snapshot. Como o
    // heap físico pode conter, para um mesmo ObjectId, tanto a versão current
    // quanto uma versão previous ainda preservada, cada registro físico é
    // comparado com o que `find_at` resolveria para aquele id — só o que
    // corresponde à localização física do registro é visitado (evita
    // duplicar ou expor a versão errada).
    [[nodiscard]] Result<void> scan_at(
        std::uint64_t snapshot_epoch,
        const std::function<Result<void>(const DecodedObject&)>& visitor);

    // Fonte de scan PREGUIÇOSA (Fase 7A): um Generator que percorre o heap
    // página a página, cedendo cada objeto visível na época do snapshot (mesma
    // regra de `scan_at`: filtra por identidade para não expor versões previous
    // nem cópias órfãs). Lê uma página de dados só quando o consumidor a
    // alcança — a base do critério TTFR (`limit 1` lê ≤ 2 páginas). `type`
    // nullopt visita todos os tipos.
    [[nodiscard]] query::Generator<Result<DecodedObject>> scan_stream(
        std::uint64_t snapshot_epoch, std::optional<TypeDefinitionId> type);
    // Páginas de dados lidas pelo scan preguiçoso desde o último reset
    // (instrumentação do critério TTFR da Fase 7A).
    [[nodiscard]] std::uint64_t data_pages_read() const noexcept {
        return data_heap_.data_pages_read();
    }
    void reset_data_pages_read() noexcept { data_heap_.reset_data_pages_read(); }

    // Baseline corrente (nullopt antes de qualquer tipo ser registrado).
    [[nodiscard]] const std::optional<Baseline>& current_baseline() const noexcept {
        return current_baseline_;
    }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return root_.epoch(); }
    // Época que a transação em andamento vai publicar: o carimbo das versões
    // que ela escreve. Vale enquanto o commit não chamou advance_epoch (as
    // escritas acontecem antes dele; há um escritor só).
    [[nodiscard]] std::uint64_t transaction_epoch() const noexcept { return root_.epoch() + 1; }
    [[nodiscard]] DatabaseUuid database_uuid() const noexcept { return root_.database_uuid(); }
    [[nodiscard]] TimelineId timeline_id() const noexcept { return root_.timeline_id(); }
    [[nodiscard]] std::uint64_t next_lsn() const noexcept { return root_.next_lsn(); }
    [[nodiscard]] std::uint64_t checkpoint_lsn() const noexcept { return root_.checkpoint_lsn(); }
    [[nodiscard]] std::uint64_t follower_ack_lsn() const noexcept {
        return root_.follower_ack_lsn();
    }
    [[nodiscard]] std::uint64_t oldest_available_lsn() const noexcept {
        return root_.oldest_available_lsn();
    }
    [[nodiscard]] Result<void> set_next_lsn(std::uint64_t next) { return root_.set_next_lsn(next); }
    [[nodiscard]] std::uint64_t checkpoint_wal_offset() const noexcept {
        return root_.checkpoint_wal_offset();
    }
    [[nodiscard]] Result<void> set_checkpoint_lsn(std::uint64_t lsn, std::uint64_t wal_offset = 0) {
        return root_.set_checkpoint_lsn(lsn, wal_offset);
    }
    [[nodiscard]] Result<void> set_follower_ack_lsn(std::uint64_t lsn) {
        return root_.set_follower_ack_lsn(lsn);
    }
    [[nodiscard]] Result<void> set_database_uuid(DatabaseUuid uuid) {
        return root_.set_database_uuid(uuid);
    }
    [[nodiscard]] Result<void> set_timeline_id(TimelineId timeline) {
        return root_.set_timeline_id(timeline);
    }
    // Total de registros físicos vivos no heap de dados (Fase 6C): inclui as
    // versões `previous` ainda preservadas e cópias órfãs ainda não coletadas.
    // Diagnóstico read-only, usado por testes e pela CLI para observar o efeito
    // do GC.
    [[nodiscard]] std::uint64_t data_record_count() const noexcept {
        return data_heap_.record_count();
    }
    // Estado de versionamento de um objeto (Fase 6): época/localização current
    // e, se houver, a versão `previous` retida. Diagnóstico read-only para a
    // CLI (`mvcc versions`).
    [[nodiscard]] Result<IdentityMap::VersionInfo> version_info(ObjectId id) const {
        return identity_.inspect(id);
    }

    // Próximo ObjectId a ser alocado (contador persistido). Exposto para o
    // rollback de transação (Fase 5) preservar a garantia de não-reuso mesmo
    // depois de reconstruir o ObjectStore a partir do disco.
    [[nodiscard]] std::uint64_t next_object_id_watermark() const noexcept {
        return root_.next_object_id();
    }
    // Garante que o contador nunca retroceda: se `at_least` for maior que o
    // valor persistido atual, avança e o grava. Seguro fora de uma transação
    // — não vaza outro campo do DBRT ainda em voo; o Database faz o flush
    // subsequente quando este caminho é usado no rollback.
    [[nodiscard]] Result<void> ensure_next_object_id_at_least(std::uint64_t at_least) {
        if (at_least <= root_.next_object_id()) {
            return {};
        }
        return root_.set_next_object_id(at_least);
    }

private:
    friend class Database;
    [[nodiscard]] Result<TypeDefinitionId> register_type(TypeDefinition definition);
    [[nodiscard]] Result<ObjectId> create_object(const TypeDefinition& type, FieldValues fields);
    // `oldest_open_snapshot_epoch` é a época do snapshot aberto mais antigo no
    // momento da escrita (nullopt se nenhum estiver aberto) — decide se esta
    // escrita conflita com uma versão `previous` ainda visível (Fase 6B).
    [[nodiscard]] Result<void> update(ObjectId id, const TypeDefinition& type, FieldValues fields,
                                      std::optional<std::uint64_t> oldest_open_snapshot_epoch);
    [[nodiscard]] Result<void> remove(ObjectId id,
                                      std::optional<std::uint64_t> oldest_open_snapshot_epoch);
    // Recicla o espaço físico das versões antigas que nenhum snapshot aberto
    // ainda pode enxergar (Fase 6C). Reconcilia cada registro do heap com a
    // identidade: a versão `current` viva nunca é tocada; uma versão `previous`
    // é liberada (e a entrada compactada com `clear_previous`) quando
    // `oldest_open_snapshot_epoch` é nulo ou >= à época `current` da entrada;
    // qualquer registro que não seja nem `current` nem a `previous` referenciada
    // é uma cópia órfã (ex.: previous sobrescrito por uma segunda alteração) e
    // é sempre liberado. Exige uma transação ativa (as liberações passam pelo
    // WAL). Devolve quantos registros físicos foram recuperados.
    [[nodiscard]] Result<std::size_t> collect_garbage(
        std::optional<std::uint64_t> oldest_open_snapshot_epoch);
    [[nodiscard]] Result<void> advance_epoch() { return root_.advance_epoch(); }
    // Verifica se uma escrita em `id` conflitaria com um snapshot ainda aberto
    // que dependa da versão `previous` (Fase 6B): só é possível quando já há
    // uma `previous` ocupada E existe um snapshot mais antigo que a época
    // `current` da entrada — nesse caso não sobra posição para a nova versão
    // que este `previous` teria de virar.
    [[nodiscard]] Result<void> check_snapshot_conflict(
        ObjectId id, std::optional<std::uint64_t> oldest_open_snapshot_epoch) const;
    // Recuperação imediata (T33.4, ADR-024): a `previous` que um update
    // vai sobrescrever, e a liberação do seu registro depois da escrita. Com
    // isso cada objeto tem no máximo duas versões físicas, sem esperar o GC.
    [[nodiscard]] Result<std::optional<storage::RecordId>> overwritten_previous(ObjectId id) const;
    [[nodiscard]] Result<void> release_overwritten(std::optional<storage::RecordId> record);
    // Cria um índice sobre (tipo, campo): monta a B+ tree, backfill dos objetos
    // atuais e registra no catálogo. Exige transação (as escritas passam pelo
    // WAL). Fase 7B.
    [[nodiscard]] Result<void> create_index(const TypeDefinition& type, FieldId field);
    // Há um índice sobre (tipo lógico, campo)?
    [[nodiscard]] bool has_index(std::string_view type_name, std::uint16_t field_id) const noexcept;
    // Igualdade/faixa via B+ tree: ObjectIds cujo valor de campo casa. Fase 7B.
    [[nodiscard]] Result<std::vector<ObjectId>> index_equal(std::string_view type_name,
                                                            std::uint16_t field_id,
                                                            const AttributeValue& key) const;
    [[nodiscard]] Result<std::vector<ObjectId>> index_range(std::string_view type_name,
                                                            std::uint16_t field_id,
                                                            const AttributeValue& lo,
                                                            const AttributeValue& hi) const;
    // Candidatos para uma consulta por índice sob `snapshot_epoch` (C2): a faixa
    // da B+ tree quando o índice serve para essa época; senão -- alguma chave
    // foi retirada do índice depois dela, e o objeto que a tinha sumiria --,
    // uma varredura dos objetos visíveis na época com o campo na faixa, na
    // mesma ordem do índice (valor, depois id). Os candidatos ainda passam
    // por `index_candidate_at`.
    [[nodiscard]] Result<std::vector<ObjectId>> index_range_at(std::string_view type_name, std::uint16_t field_id,
                                                               const AttributeValue& lo, const AttributeValue& hi,
                                                               std::uint64_t snapshot_epoch);
    // O índice (tipo, campo) ainda tem todas as chaves que um snapshot desta
    // época enxerga?
    [[nodiscard]] bool index_serves_epoch(std::string_view type_name, std::uint16_t field_id,
                                          std::uint64_t snapshot_epoch) const noexcept;
    // Revalida um candidato do índice contra a versão visível no snapshot (Fase
    // 7B): devolve o objeto na época `snapshot_epoch` só se o valor do campo
    // `field_id` estiver em [lo, hi] nessa versão; nullopt se não visível
    // (criado/removido depois) ou fora da faixa. O índice reflete o estado
    // corrente, então esta checagem evita falso-positivo sob o snapshot.
    [[nodiscard]] Result<std::optional<DecodedObject>> index_candidate_at(
        ObjectId id, std::uint64_t snapshot_epoch, std::uint16_t field_id, const AttributeValue& lo,
        const AttributeValue& hi);
    // Mantém todos os índices do tipo após uma escrita: `insert=true` adiciona a
    // entrada do objeto, `false` a remove. Nulos não são indexados.
    [[nodiscard]] Result<void> index_maintain(const std::string& type_name, ObjectId id,
                                              const FieldValues& fields, bool insert);

    ObjectStore(storage::PageFile& file, DatabaseRoot root, IdentityMap identity,
                storage::TableHeap data_heap, CatalogStore catalog, TypeRegistry registry,
                std::vector<TypeDefinitionId> type_ids, std::vector<Baseline> baselines,
                std::optional<Baseline> baseline,
                std::optional<IndexCatalog> indexes = std::nullopt) noexcept
        : file_{&file},
          root_{std::move(root)},
          identity_{std::move(identity)},
          data_heap_{std::move(data_heap)},
          catalog_{std::move(catalog)},
          registry_{std::move(registry)},
          type_ids_{std::move(type_ids)},
          baselines_{std::move(baselines)},
          current_baseline_{std::move(baseline)},
          indexes_{std::move(indexes)},
          opened_epoch_{root_.epoch()} {}

    // Aloca o próximo ObjectId, persistindo o contador antes de devolvê-lo.
    [[nodiscard]] Result<ObjectId> allocate_object_id();

    // Arquivo cuja vida é controlada pelo chamador.
    storage::PageFile* file_;
    DatabaseRoot root_;
    IdentityMap identity_;
    storage::TableHeap data_heap_;
    CatalogStore catalog_;
    TypeRegistry registry_;
    // Ids das versões ativas, usados ao construir a próxima baseline.
    std::vector<TypeDefinitionId> type_ids_;
    // Baselines históricas imutáveis, inclusive a corrente.
    std::vector<Baseline> baselines_;
    std::optional<Baseline> current_baseline_;
    // Diretório de índices (Fase 7B); nullopt até o primeiro índice ser criado.
    std::optional<IndexCatalog> indexes_;
    // C2: época da última retirada de chave de cada índice (por slot do
    // catálogo). Retiradas de antes da abertura não são conhecidas: valem como
    // `opened_epoch_` (snapshots anteriores à abertura -- só existem depois de
    // um rollback, que relê o store -- usam a varredura).
    std::unordered_map<std::size_t, std::uint64_t> index_removed_epoch_;
    std::uint64_t opened_epoch_{0};

    // Último registro lido por `peek_type`, para `get` do MESMO id não repetir a
    // resolução de identidade e a leitura do heap.
    //
    // Existe porque o par natural `Database::get<T>()` + `materialize()` fazia
    // duas leituras completas do mesmo objeto: `get` chama `peek_type` (resolve
    // o id, lê o registro, decodifica só o cabeçalho para validar o tipo) e
    // `materialize` chama `get` (resolve o id, lê o registro, decodifica tudo).
    // O profiler mostrou `identity_lookup` e `heap_record_read` com
    // `calls_per_operation = 2,00` na fase de leitura
    // (docs-process/PLANO_PROFILER.md §9.2.4). A Fase 10C já havia eliminado o
    // decode duplicado; a leitura duplicada ficou.
    //
    // Uma entrada só, e o `epoch` é o que a torna correta em vez de perigosa:
    // qualquer commit avança a época (`advance_epoch`), então um registro
    // cacheado por uma época anterior nunca é servido. Leituras por snapshot
    // (`get_at`/`find_at`) não passam por aqui.
    //
    // A entrada é por thread (`thread_local` em object_store.cpp, C6.3): era um
    // membro, e duas threads lendo ao mesmo tempo gravavam nele. `cache_tag_`
    // diz de qual store ela é -- único por instância, então um store reaberto
    // (rollback) ou outro banco no mesmo endereço nunca aproveita a de outro.
    std::uint64_t cache_tag_{next_cache_tag()};
    [[nodiscard]] static std::uint64_t next_cache_tag() noexcept;
};

} // namespace modb::object
