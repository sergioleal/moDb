#include "modb/net/probe.hpp"
#include "modb/net/server.hpp"
#include "modb/object/binding.hpp"
#include "modb/object/database.hpp"

#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

using namespace modb;
using namespace modb::net;
using namespace modb::object;

namespace {

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string_view suffix) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("modb-13d-" + std::to_string(unique) + "-" + std::string{suffix} + ".modb");
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

struct Item {
    std::string name;
};

BindingBuilder<Item> item_binding() {
    BindingBuilder<Item> builder{"Item"};
    builder.field<1>("name", &Item::name);
    return builder;
}

} // namespace

int main() {
    TestSuite suite;
    TemporaryDatabase temp{"lifecycle"};
    const auto ready = std::filesystem::temp_directory_path() /
                       ("modb-ready-" + temp.path().filename().string());
    const auto live = std::filesystem::temp_directory_path() /
                      ("modb-live-" + temp.path().filename().string());

    // Recovery no cold start: commit, "kill" (fechar sem sync especial), reabrir.
    ObjectId committed{};
    {
        auto created = Database::create(temp.path());
        suite.check(created.has_value(), "create");
        if (!created) {
            return suite.finish();
        }
        auto database = std::make_shared<Database>(std::move(*created));
        auto id = DatabaseRegistry::instance().attach(database);
        suite.check(id && database->bind(item_binding()).has_value(), "bind");
        auto tx = database->begin();
        auto handle = database->create(*tx, Item{"durable"});
        suite.check(handle && tx->commit().has_value(), "commit");
        committed = handle->id();
        DatabaseRegistry::instance().detach(*id);
        database.reset();
    }

    {
        auto server = Server::listen(temp.path(), "127.0.0.1", 0);
        suite.check(server.has_value(), "listen after reopen/recovery");
        if (!server) {
            return suite.finish();
        }
        suite.check(write_probe_file(ready).has_value(), "ready probe");
        suite.check(write_probe_file(live, "live\n").has_value(), "live probe");
        suite.check(std::filesystem::exists(ready), "ready exists");

        suite.check(server->database().bind(item_binding()).has_value(), "rebind after recovery");
        auto snap = server->database().snapshot();
        suite.check(snap.has_value(), "snapshot");
        auto loaded = server->database().get<Item>(committed, *snap);
        suite.check(loaded.has_value() && loaded->name == "durable",
                    "committed visible after recovery");

        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            server->request_stop();
        });
        auto loop = server->serve_forever();
        stopper.join();
        suite.check(loop.has_value(), "serve_forever stops cleanly");
        suite.check(server->stop_requested(), "stop flag set");
        suite.check(remove_probe_file(ready).has_value(), "clear ready");
        suite.check(!std::filesystem::exists(ready), "ready removed on shutdown");
    }

    std::error_code ignored;
    std::filesystem::remove(live, ignored);
    return suite.finish();
}
