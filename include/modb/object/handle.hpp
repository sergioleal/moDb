#pragma once

// Importa Result e os identificadores.
#include "modb/error.hpp"
#include "modb/object/ids.hpp"

#include <type_traits>

namespace modb::object {

// Declara Database para a amizade que protege o construtor.
class Database;
class Transaction;

template <typename>
struct member_pointer_traits;

template <typename Member, typename Owner>
struct member_pointer_traits<Member Owner::*> {
    using member_type = Member;
};

template <typename MemberPointer>
using member_type_t = typename member_pointer_traits<MemberPointer>::member_type;

// Referência leve e tipada a um objeto persistente: apenas identidade
// (DatabaseId + ObjectId), como manda arquitetura.md §6.
template <typename T>
class Handle {
public:
    [[nodiscard]] DatabaseId database() const noexcept { return database_; }
    [[nodiscard]] ObjectId id() const noexcept { return id_; }

    template <auto Member>
    [[nodiscard]] Result<member_type_t<decltype(Member)>> get() const;

    template <auto Member, typename V>
    [[nodiscard]] Result<void> set(Transaction& transaction, V&& value) const;

    friend bool operator==(Handle, Handle) = default;

private:
    // Só o Database cria handles, garantindo que o id veio de uma operação real.
    friend class Database;
    explicit Handle(DatabaseId database, ObjectId id) noexcept
        : database_{database}, id_{id} {}

    DatabaseId database_;
    ObjectId id_;
};

} // namespace modb::object
