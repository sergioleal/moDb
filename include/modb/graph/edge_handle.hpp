#pragma once

// EdgeHandle: visão runtime tipada de uma aresta Ref/OwnedRef (Fase 12A / ADR-015).
// Não é persistido — o arquivo continua gravando apenas Ref<T> / OwnedRef<T>.

#include "modb/error.hpp"
#include "modb/object/database.hpp"
#include "modb/object/ids.hpp"
#include "modb/object/ref.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace modb::graph {

enum class EdgeKind : std::uint8_t {
    association = 0,
    ownership = 1,
};

template <class From, class To, EdgeKind Kind = EdgeKind::association>
class EdgeHandle {
public:
    [[nodiscard]] object::DatabaseId database() const noexcept { return database_; }
    [[nodiscard]] object::ObjectId source_id() const noexcept { return source_id_; }
    [[nodiscard]] object::ObjectId target_id() const noexcept { return target_id_; }
    [[nodiscard]] object::FieldId field() const noexcept { return field_; }

    [[nodiscard]] Result<From> source(const object::Snapshot& snapshot) const {
        auto database = object::DatabaseRegistry::instance().find(database_);
        if (!database) {
            return std::unexpected(database.error());
        }
        return (*database)->get<From>(source_id_, snapshot);
    }

    [[nodiscard]] Result<To> target(const object::Snapshot& snapshot) const {
        auto database = object::DatabaseRegistry::instance().find(database_);
        if (!database) {
            return std::unexpected(database.error());
        }
        auto resolved = (*database)->get<To>(target_id_, snapshot);
        if (!resolved && resolved.error().code == ErrorCode::record_not_found) {
            return std::unexpected(Error{ErrorCode::edge_target_not_found,
                                         "edge target not found: " +
                                             std::to_string(target_id_.value)});
        }
        return resolved;
    }

    // true se o alvo não existe sob o snapshot; outros erros propagam.
    [[nodiscard]] Result<bool> dangling(const object::Snapshot& snapshot) const {
        auto resolved = target(snapshot);
        if (resolved) {
            return false;
        }
        if (resolved.error().code == ErrorCode::edge_target_not_found) {
            return true;
        }
        return std::unexpected(resolved.error());
    }

private:
    template <class F, class T>
    friend Result<EdgeHandle<F, T, EdgeKind::association>> edge(
        object::Database& database, const object::Handle<F>& source, object::FieldId field,
        object::Ref<T> F::* member);

    template <class F, class T>
    friend Result<EdgeHandle<F, T, EdgeKind::ownership>> edge(
        object::Database& database, const object::Handle<F>& source, object::FieldId field,
        object::OwnedRef<T> F::* member);

    EdgeHandle(object::DatabaseId database, object::ObjectId source, object::ObjectId target,
               object::FieldId field) noexcept
        : database_{database}, source_id_{source}, target_id_{target}, field_{field} {}

    object::DatabaseId database_{};
    object::ObjectId source_id_{};
    object::ObjectId target_id_{};
    object::FieldId field_{};
};

namespace detail {

template <class From, EdgeKind Kind>
[[nodiscard]] Result<const object::FieldBinder*> validate_edge_field(
    object::Database& database, object::FieldId field) {
    auto binder = database.find_bound_field<From>(field);
    if (!binder) {
        return std::unexpected(Error{ErrorCode::invalid_edge, binder.error().message});
    }
    if ((*binder)->is_embedded || (*binder)->type != object::AttributeType::ref) {
        return std::unexpected(
            Error{ErrorCode::invalid_edge, "field is not a Ref/OwnedRef relationship"});
    }
    constexpr bool expect_owned = Kind == EdgeKind::ownership;
    if ((*binder)->is_owned != expect_owned) {
        return std::unexpected(Error{ErrorCode::invalid_edge,
                                     expect_owned ? "field is association, not ownership"
                                                  : "field is ownership, not association"});
    }
    return *binder;
}

} // namespace detail

template <class From, class To>
[[nodiscard]] Result<EdgeHandle<From, To, EdgeKind::association>> edge(
    object::Database& database, const object::Handle<From>& source, object::FieldId field,
    object::Ref<To> From::* member) {
    auto binder = detail::validate_edge_field<From, EdgeKind::association>(database, field);
    if (!binder) {
        return std::unexpected(binder.error());
    }
    auto handle = database.get<From>(source.id());
    if (!handle) {
        return std::unexpected(handle.error());
    }
    if (handle->database() != source.database()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "source handle belongs to another database"});
    }
    auto value = database.materialize(*handle);
    if (!value) {
        return std::unexpected(value.error());
    }
    const object::ObjectId member_target = ((*value).*member).target;
    auto loaded = (*binder)->load(&*value);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    auto loaded_target = loaded->as_ref();
    if (!loaded_target) {
        return std::unexpected(Error{ErrorCode::invalid_edge, "field binder did not yield Ref"});
    }
    if (*loaded_target != member_target) {
        return std::unexpected(
            Error{ErrorCode::invalid_edge, "FieldId does not match relationship member"});
    }
    return EdgeHandle<From, To, EdgeKind::association>{source.database(), source.id(),
                                                       member_target, field};
}

template <class From, class To>
[[nodiscard]] Result<EdgeHandle<From, To, EdgeKind::ownership>> edge(
    object::Database& database, const object::Handle<From>& source, object::FieldId field,
    object::OwnedRef<To> From::* member) {
    auto binder = detail::validate_edge_field<From, EdgeKind::ownership>(database, field);
    if (!binder) {
        return std::unexpected(binder.error());
    }
    auto handle = database.get<From>(source.id());
    if (!handle) {
        return std::unexpected(handle.error());
    }
    if (handle->database() != source.database()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "source handle belongs to another database"});
    }
    auto value = database.materialize(*handle);
    if (!value) {
        return std::unexpected(value.error());
    }
    const object::ObjectId member_target = ((*value).*member).target;
    auto loaded = (*binder)->load(&*value);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    auto loaded_target = loaded->as_ref();
    if (!loaded_target) {
        return std::unexpected(Error{ErrorCode::invalid_edge, "field binder did not yield Ref"});
    }
    if (*loaded_target != member_target) {
        return std::unexpected(
            Error{ErrorCode::invalid_edge, "FieldId does not match relationship member"});
    }
    return EdgeHandle<From, To, EdgeKind::ownership>{source.database(), source.id(), member_target,
                                                     field};
}

} // namespace modb::graph
