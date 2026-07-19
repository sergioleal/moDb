#pragma once

// GraphView: adjacência tipada sob um Snapshot (Fase 12B / ADR-015).
// Outgoing em PersistentVector<Ref<T>>; incoming só com índice no campo Ref.

#include "modb/error.hpp"
#include "modb/graph/edge_handle.hpp"
#include "modb/object/attribute_value.hpp"
#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"
#include "modb/object/ids.hpp"
#include "modb/object/ref.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modb::graph {

enum class EdgeDirection : std::uint8_t {
    outgoing = 0,
    incoming = 1,
    both = 2,
};

[[nodiscard]] constexpr std::string_view to_string(EdgeDirection direction) noexcept {
    switch (direction) {
    case EdgeDirection::outgoing:
        return "outgoing";
    case EdgeDirection::incoming:
        return "incoming";
    case EdgeDirection::both:
        return "both";
    }
    return "unknown";
}

// Visão runtime ancorada num Snapshot (não o possui). O Snapshot deve
// permanecer vivo enquanto a view for usada.
class GraphView {
public:
    GraphView(object::Database& database, const object::Snapshot& snapshot)
        : database_{&database}, snapshot_{&snapshot} {}

    [[nodiscard]] object::DatabaseId database_id() const noexcept {
        return snapshot_->database();
    }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return snapshot_->epoch(); }
    [[nodiscard]] const object::Snapshot& snapshot() const noexcept { return *snapshot_; }

    // Arestas de saída a partir de PersistentVector<Ref<To>> (BlobId no pai).
    // Preserva a ordem dos elementos da coleção.
    template <class From, class To>
    [[nodiscard]] Result<std::vector<EdgeHandle<From, To, EdgeKind::association>>>
    outgoing_collection(const object::Handle<From>& source, object::FieldId field,
                        object::BlobId From::* member) const {
        if (source.database() != snapshot_->database()) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "source handle belongs to another database"});
        }
        auto binder = database_->find_bound_field<From>(field);
        if (!binder) {
            return std::unexpected(Error{ErrorCode::invalid_edge, binder.error().message});
        }
        if ((*binder)->type != object::AttributeType::blob || (*binder)->is_embedded) {
            return std::unexpected(
                Error{ErrorCode::invalid_edge, "field is not a PersistentVector blob"});
        }

        auto value = database_->get<From>(source.id(), *snapshot_);
        if (!value) {
            return std::unexpected(value.error());
        }
        const object::BlobId blob = ((*value).*member);
        auto loaded = (*binder)->load(&*value);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        auto loaded_blob = loaded->as_blob();
        if (!loaded_blob) {
            return std::unexpected(
                Error{ErrorCode::invalid_edge, "field binder did not yield BlobId"});
        }
        if (*loaded_blob != blob) {
            return std::unexpected(
                Error{ErrorCode::invalid_edge, "FieldId does not match collection member"});
        }
        if (blob.value == 0) {
            return std::vector<EdgeHandle<From, To, EdgeKind::association>>{};
        }

        auto blobs = database_->blobs();
        object::PersistentVector<object::Ref<To>> vector{blobs, blob};
        std::vector<EdgeHandle<From, To, EdgeKind::association>> edges;
        auto status = vector.for_each([&](const object::Ref<To>& ref) -> Result<void> {
            edges.push_back(make_edge_handle<From, To, EdgeKind::association>(
                source.database(), source.id(), ref.target, field));
            return {};
        });
        if (!status) {
            return std::unexpected(status.error());
        }
        return edges;
    }

    // Arestas de entrada: objetos From cujo campo Ref aponta para `target`.
    // Exige índice B+ no campo; sem índice → invalid_edge (sem scan reverso).
    template <class From, class To>
    [[nodiscard]] Result<std::vector<EdgeHandle<From, To, EdgeKind::association>>> incoming(
        const object::Handle<To>& target, object::FieldId field,
        object::Ref<To> From::* member) const {
        if (target.database() != snapshot_->database()) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "target handle belongs to another database"});
        }
        auto binder = detail::validate_edge_field<From, EdgeKind::association>(*database_, field);
        if (!binder) {
            return std::unexpected(binder.error());
        }

        auto ids = database_->indexed_object_ids<From>(field, object::AttributeValue{target.id()});
        if (!ids) {
            return std::unexpected(ids.error());
        }

        std::vector<EdgeHandle<From, To, EdgeKind::association>> edges;
        edges.reserve(ids->size());
        for (const object::ObjectId source_id : *ids) {
            auto source = database_->get<From>(source_id, *snapshot_);
            if (!source) {
                if (source.error().code == ErrorCode::record_not_found) {
                    continue; // não visível nesta época
                }
                return std::unexpected(source.error());
            }
            const object::ObjectId pointed = ((*source).*member).target;
            if (pointed != target.id()) {
                continue; // chave de índice desatualizada vs. snapshot
            }
            edges.push_back(make_edge_handle<From, To, EdgeKind::association>(
                target.database(), source_id, target.id(), field));
        }
        return edges;
    }

private:
    object::Database* database_{};
    const object::Snapshot* snapshot_{};
};

[[nodiscard]] inline Result<GraphView> open_graph_view(object::Database& database,
                                                       const object::Snapshot& snapshot) {
    if (snapshot.database().value == 0) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "snapshot is not attached to a database"});
    }
    auto resolved = object::DatabaseRegistry::instance().find(snapshot.database());
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    if (resolved->get() != &database) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "snapshot belongs to a different database"});
    }
    return GraphView{database, snapshot};
}

} // namespace modb::graph
