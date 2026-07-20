#include "modb/object/database.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <system_error>

using namespace modb;
using namespace modb::object;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-14-id-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
    }
    ~TemporaryDatabase() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + ".wal", ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

int main() {
    TestSuite suite;
    TemporaryDatabase temp{"uuid"};

    DatabaseUuid first{};
    {
        auto created = Database::create(temp.path());
        suite.check(created.has_value(), "create");
        if (!created) {
            return suite.finish();
        }
        first = created->database_uuid();
        suite.check(!first.is_nil(), "uuid assigned on create");
        suite.check(created->timeline_id().value == 1, "timeline starts at 1");
        suite.check(created->next_lsn() == 1, "next_lsn starts at 1");
    }

    {
        auto opened = Database::open(temp.path());
        suite.check(opened.has_value(), "reopen");
        if (opened) {
            suite.check(opened->database_uuid() == first, "uuid survives reopen");
            suite.check(opened->timeline_id().value == 1, "timeline survives reopen");
            suite.check(opened->next_lsn() == 1, "next_lsn survives reopen");
        }
    }

    return suite.finish();
}
