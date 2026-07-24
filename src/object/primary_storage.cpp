#include "modb/object/primary_storage.hpp"

namespace modb::object {

Result<PrimaryStorage> parse_primary_storage(std::string_view text) {
    if (text == "full") {
        return PrimaryStorage::full;
    }
    if (text == "wal_only") {
        return PrimaryStorage::wal_only;
    }
    return std::unexpected(
        Error{ErrorCode::invalid_argument, "primary_storage must be full|wal_only"});
}

} // namespace modb::object
