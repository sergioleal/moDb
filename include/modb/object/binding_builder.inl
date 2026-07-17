#pragma once

// Implementação de BindingBuilder<T>::build (template, precisa ficar no header).

// Disponibiliza os conjuntos usados na validação de unicidade.
#include <string>
#include <unordered_set>

namespace modb::object {

template <typename T>
Result<Binding> BindingBuilder<T>::build() {
    // Um binding sem campos não descreve nada de útil.
    if (fields_.empty()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "binding must have at least one field"});
    }
    // Detecta FieldIds e nomes repetidos, e o id reservado zero.
    std::unordered_set<std::uint16_t> ids;
    std::unordered_set<std::string> names;
    for (const auto& binder : fields_) {
        if (binder.id.value == 0) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "FieldId must not be zero: " + binder.name});
        }
        if (!ids.insert(binder.id.value).second) {
            return std::unexpected(Error{ErrorCode::duplicate_field,
                                         "duplicate FieldId in binding: " + binder.name});
        }
        if (!names.insert(binder.name).second) {
            return std::unexpected(
                Error{ErrorCode::duplicate_column, "duplicate field name in binding: " + binder.name});
        }
    }
    return Binding{std::move(type_name_), std::move(fields_)};
}

} // namespace modb::object
