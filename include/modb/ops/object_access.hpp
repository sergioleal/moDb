#pragma once

// Fachada de objetos para módulos de domínio (Fase 9 / ADR-012).
// Expõe get/create/update/remove tipados — nunca páginas, WAL ou índices.

#include "modb/error.hpp"
#include "modb/object/database.hpp"
#include "modb/object/ids.hpp"

#include <optional>
#include <utility>

namespace modb::ops {

class ObjectAccess {
public:
    ObjectAccess(object::Database& database, object::Transaction* transaction,
                 object::Snapshot* snapshot)
        : database_{&database}, transaction_{transaction}, snapshot_{snapshot} {}

    [[nodiscard]] object::Database& database() const noexcept { return *database_; }
    [[nodiscard]] bool writable() const noexcept { return transaction_ != nullptr; }

    template <typename T>
    [[nodiscard]] Result<object::Handle<T>> create(const T& value) {
        if (transaction_ == nullptr) {
            return std::unexpected(
                Error{ErrorCode::transaction_required, "ObjectAccess create requires write mode"});
        }
        return database_->create(*transaction_, value);
    }

    template <typename T>
    [[nodiscard]] Result<object::Handle<T>> get(object::ObjectId id) {
        return database_->get<T>(id);
    }

    // Leitura tipada: sob Snapshot usa a época fixa; senão materializa o corrente.
    template <typename T>
    [[nodiscard]] Result<T> read(object::ObjectId id) {
        if (snapshot_ != nullptr) {
            return database_->get<T>(id, *snapshot_);
        }
        auto handle = database_->get<T>(id);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        return database_->materialize(*handle);
    }

    template <typename T>
    [[nodiscard]] Result<T> materialize(const object::Handle<T>& handle) {
        return database_->materialize(handle);
    }

    template <typename T>
    [[nodiscard]] Result<void> update(const object::Handle<T>& handle, const T& value) {
        if (transaction_ == nullptr) {
            return std::unexpected(
                Error{ErrorCode::transaction_required, "ObjectAccess update requires write mode"});
        }
        return database_->update(*transaction_, handle, value);
    }

    [[nodiscard]] Result<void> remove(object::ObjectId id) {
        if (transaction_ == nullptr) {
            return std::unexpected(
                Error{ErrorCode::transaction_required, "ObjectAccess remove requires write mode"});
        }
        return database_->remove(*transaction_, id);
    }

    [[nodiscard]] object::Transaction& transaction() {
        return *transaction_;
    }

private:
    object::Database* database_{nullptr};
    object::Transaction* transaction_{nullptr};
    object::Snapshot* snapshot_{nullptr};
};

} // namespace modb::ops
