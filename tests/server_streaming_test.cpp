#include "modb/net/client.hpp"
#include "modb/net/native_socket.hpp"
#include "modb/net/protocol.hpp"
#include "modb/net/server.hpp"
#include "modb/object/database.hpp"
#include "modb/object/object_codec.hpp"

#include "test_support.hpp"

#include <chrono>
#include <coroutine>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

std::filesystem::path temp_db_path(const char* tag) {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("modb-8f-" + std::string{tag} + "-" + std::to_string(stamp) + ".modb");
}

struct Item {
    std::string name;
    std::int64_t value{};
    friend bool operator==(const Item&, const Item&) = default;
};

modb::object::BindingBuilder<Item> item_builder() {
    modb::object::BindingBuilder<Item> builder{"Item"};
    builder.field<1>("name", &Item::name).field<2>("value", &Item::value);
    return builder;
}

std::shared_ptr<modb::object::Database> share(modb::Result<modb::object::Database>& result) {
    if (!result) {
        return {};
    }
    return std::make_shared<modb::object::Database>(std::move(*result));
}

modb::Result<void> seed(modb::object::Database& database, int count, int batch = 2'000) {
    for (int start = 0; start < count; start += batch) {
        auto tx = database.begin();
        if (!tx) {
            return std::unexpected(tx.error());
        }
        for (int i = start; i < start + batch && i < count; ++i) {
            auto handle = database.create(*tx, Item{"item-" + std::to_string(i), i});
            if (!handle) {
                return std::unexpected(handle.error());
            }
        }
        if (auto committed = tx->commit(); !committed) {
            return std::unexpected(committed.error());
        }
    }
    return {};
}

void cleanup(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".wal", ignored);
}

// Coroutine mínima para co_await ObjectStream::await_next() (Fase 8E).
struct VoidTask {
    struct promise_type {
        VoidTask get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };
};

VoidTask collect_await(modb::net::ObjectStream& stream,
                       std::vector<modb::object::DecodedObject>& out) {
    while (true) {
        auto next = co_await stream.await_next();
        if (!next) {
            co_return;
        }
        if (!*next) {
            co_return;
        }
        out.push_back(std::move(**next));
    }
}

} // namespace

int main() {
    TestSuite suite;
    using modb::ErrorCode;
    using modb::net::Client;
    using modb::net::Compression;
    using modb::net::NativeSocket;
    using modb::net::QueryDescription;
    using modb::net::Server;
    using modb::net::protocol_version;
    using modb::object::AttributeValue;
    using modb::object::Database;
    using modb::object::DatabaseRegistry;
    using modb::object::DecodedObject;
    using modb::object::FieldId;
    using modb::object::encode_object_payload;

    // --- 8B: handshake ainda funciona (cliente fecha sem Query) ---
    {
        const auto path = temp_db_path("handshake");
        cleanup(path);
        {
            auto created = Database::create(path);
            suite.check(created.has_value(), "create handshake database");
        }
        {
            auto server = Server::listen(path, "127.0.0.1", 0);
            suite.check(server.has_value(), "handshake server listens");
            if (server) {
                const auto port = server->port();
                const auto db_name = std::string{server->database_name()};
                std::thread acceptor([&server, &suite] {
                    suite.check(server->serve_one().has_value(), "serve_one after close is ok");
                });
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                // Client::handshake destrói o cliente internamente.
                auto hello_ok = Client::handshake("127.0.0.1", port, db_name);
                suite.check(hello_ok.has_value(), "client handshake succeeds");
                if (hello_ok) {
                    suite.check(hello_ok->version == protocol_version, "HelloOk version matches");
                    suite.check(hello_ok->selected_codec == Compression::rle,
                                "HelloOk selects compression=rle when client accepts it");
                    suite.check(hello_ok->max_concurrent_streams >= 1,
                                "HelloOk advertises concurrent stream limit");
                    suite.check(hello_ok->max_expansion_ratio >= 1,
                                "HelloOk advertises expansion ratio");
                    suite.check(hello_ok->idle_timeout_ms > 0, "HelloOk advertises idle timeout");
                }
                acceptor.join();
            }
        }
        cleanup(path);
    }

    // --- 8C: fluxo de 10 mil objetos ---
    constexpr int total = 10'000;
    const auto path = temp_db_path("stream");
    cleanup(path);

    modb::object::TypeDefinitionId type_id{};
    {
        auto created = Database::create(path);
        auto database = share(created);
        suite.check(database != nullptr, "stream database created");
        if (!database) {
            return suite.finish();
        }
        auto attached = DatabaseRegistry::instance().attach(database);
        suite.check(attached.has_value(), "stream database attached");
        suite.check(database->bind(item_builder()).has_value(), "Item bound");
        auto tid = database->type_id_of<Item>();
        suite.check(tid.has_value(), "type id available");
        if (!tid) {
            return suite.finish();
        }
        type_id = *tid;
        suite.check(seed(*database, total).has_value(), "10k objects seeded");
        DatabaseRegistry::instance().detach(*attached);
    }

    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "stream server listens");
        if (!server) {
            return suite.finish();
        }
        const auto port = server->port();
        const auto db_name = std::string{server->database_name()};

        std::thread acceptor([&server, &suite] {
            suite.check(server->serve_one().has_value(), "serve_one streams query");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        {
            auto client = Client::connect("127.0.0.1", port, db_name);
            suite.check(client.has_value(), "client connects for streaming");
            if (client) {
                auto stream = client->query(QueryDescription{.type = type_id});
                suite.check(stream.has_value(), "remote query starts");
                if (stream) {
                    std::vector<std::int64_t> values;
                    values.reserve(total);
                    bool order_ok = true;
                    bool payload_ok = true;
                    while (true) {
                        auto next = stream->next();
                        if (!next) {
                            suite.check(false, "stream next failed unexpectedly");
                            break;
                        }
                        if (!*next) {
                            break;
                        }
                        const auto& object = **next;
                        suite.check(object.type == type_id, "envelope type matches");
                        if (object.fields.size() != 2) {
                            payload_ok = false;
                        }
                        std::int64_t value = -1;
                        for (const auto& [field, attr] : object.fields) {
                            if (field.value == 2) {
                                auto as_int = attr.as_int64();
                                if (as_int) {
                                    value = *as_int;
                                }
                            }
                        }
                        if (!values.empty() && value < values.back()) {
                            order_ok = false;
                        }
                        values.push_back(value);

                        if (!encode_object_payload(object.fields)) {
                            payload_ok = false;
                        }
                    }

                    suite.check(static_cast<int>(values.size()) == total,
                                "received exactly 10k objects");
                    suite.check(order_ok, "production order preserved");
                    suite.check(payload_ok, "logical payloads round-trip without physical ids");
                }
            }
        } // client destruído → socket fechado → sessão do servidor termina
        acceptor.join();
    }

    // --- igualdade + limite + projeção ---
    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "filter server listens");
        if (server) {
            const auto port = server->port();
            const auto db_name = std::string{server->database_name()};
            std::thread acceptor([&server, &suite] {
                suite.check(server->serve_one().has_value(), "filtered serve_one ok");
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            {
                auto client = Client::connect("127.0.0.1", port, db_name);
                suite.check(client.has_value(), "filter client connects");
                if (client) {
                    QueryDescription description{
                        .type = type_id,
                        .limit = 3,
                        .equals =
                            modb::net::EqualityFilter{FieldId{2}, AttributeValue{std::int64_t{42}}},
                        .project = {FieldId{2}},
                    };
                    auto stream = client->query(description);
                    suite.check(stream.has_value(), "filtered query starts");
                    if (stream) {
                        std::size_t count = 0;
                        bool projected = true;
                        while (true) {
                            auto next = stream->next();
                            if (!next || !*next) {
                                if (next && !*next) {
                                    break;
                                }
                                suite.check(false, "filtered stream error");
                                break;
                            }
                            ++count;
                            if ((**next).fields.size() != 1 ||
                                (**next).fields[0].first.value != 2) {
                                projected = false;
                            }
                        }
                        suite.check(count == 1, "equals+limit returns the matching object");
                        suite.check(projected, "projection keeps only requested fields");
                    }
                }
            }
            acceptor.join();
        }
    }

    // --- falha após N ---
    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "failpoint server listens");
        if (server) {
            constexpr std::size_t fail_after = 7;
            server->fail_stream_after(fail_after);
            const auto port = server->port();
            const auto db_name = std::string{server->database_name()};
            std::thread acceptor([&server, &suite] {
                suite.check(server->serve_one().has_value(), "failpoint serve_one completes");
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            {
                auto client = Client::connect("127.0.0.1", port, db_name);
                suite.check(client.has_value(), "failpoint client connects");
                if (client) {
                    auto stream = client->query(QueryDescription{.type = type_id});
                    suite.check(stream.has_value(), "failpoint query starts");
                    if (stream) {
                        std::size_t count = 0;
                        bool saw_error = false;
                        bool io_error = false;
                        while (true) {
                            auto next = stream->next();
                            if (!next) {
                                saw_error = true;
                                io_error = next.error().code == ErrorCode::io_error;
                                break;
                            }
                            if (!*next) {
                                break;
                            }
                            ++count;
                        }
                        suite.check(count == fail_after, "exactly N objects before StreamError");
                        suite.check(saw_error && io_error, "StreamError surfaces as io_error");
                    }
                }
            }
            acceptor.join();
        }
    }

    // --- realocação física não altera bytes lógicos ---
    std::vector<std::byte> before_payload;
    {
        auto reopened = Database::open(path);
        auto database = share(reopened);
        suite.check(database != nullptr, "reopen for physical independence");
        if (database) {
            auto attached = DatabaseRegistry::instance().attach(database);
            suite.check(database->bind(item_builder()).has_value(), "rebind Item");
            modb::object::ObjectId target{};
            for (auto& object : database->query_objects(Database::ObjectQuerySpec{
                     .type = type_id,
                     .equals = std::pair{FieldId{2}, AttributeValue{std::int64_t{100}}},
                 })) {
                suite.check(object.has_value(), "locate decoded object");
                if (object) {
                    target = object->id;
                    auto encoded = encode_object_payload(object->fields);
                    suite.check(encoded.has_value(), "encode before update");
                    if (encoded) {
                        before_payload = std::move(*encoded);
                    }
                }
                break;
            }
            auto before_version = database->version_info(target);
            suite.check(before_version.has_value(), "version before update");
            {
                auto tx = database->begin();
                suite.check(tx.has_value(), "begin update tx");
                if (tx) {
                    auto handle = database->get<Item>(target);
                    suite.check(handle.has_value(), "handle for update");
                    if (handle) {
                        auto updated = database->update(*tx, *handle, Item{"item-100", 100});
                        suite.check(updated.has_value(), "logical update applied");
                    }
                    suite.check(tx->commit().has_value(), "update committed");
                }
            }
            auto after_version = database->version_info(target);
            suite.check(after_version.has_value(), "version after update");
            if (before_version && after_version) {
                suite.check(before_version->current != after_version->current,
                            "physical RecordId changed after update");
            }
            DatabaseRegistry::instance().detach(*attached);
        }
    }

    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        if (server) {
            const auto port = server->port();
            const auto db_name = std::string{server->database_name()};
            std::thread acceptor([&server] { (void)server->serve_one(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            {
                auto client = Client::connect("127.0.0.1", port, db_name);
                if (client) {
                    auto stream = client->query(QueryDescription{
                        .type = type_id,
                        .equals = modb::net::EqualityFilter{FieldId{2},
                                                            AttributeValue{std::int64_t{100}}},
                    });
                    if (stream) {
                        auto next = stream->next();
                        suite.check(next.has_value() && next->has_value(),
                                    "object after relocation");
                        if (next && *next) {
                            auto after_payload = encode_object_payload((**next).fields);
                            suite.check(after_payload.has_value() && *after_payload == before_payload,
                                        "logical payload unchanged after physical relocation");
                        }
                        while (true) {
                            auto rest = stream->next();
                            if (!rest || !*rest) {
                                break;
                            }
                        }
                    }
                }
            }
            acceptor.join();
        }
    }

    // --- 8D: backpressure com cliente lento (1 obj/50 ms) ---
    {
        constexpr int slow_total = 80;
        const auto slow_path = temp_db_path("backpressure");
        cleanup(slow_path);
        modb::object::TypeDefinitionId slow_type{};
        {
            auto created = Database::create(slow_path);
            auto database = share(created);
            suite.check(database != nullptr, "backpressure database created");
            if (!database) {
                return suite.finish();
            }
            auto attached = DatabaseRegistry::instance().attach(database);
            suite.check(database->bind(item_builder()).has_value(), "backpressure Item bound");
            auto tid = database->type_id_of<Item>();
            suite.check(tid.has_value(), "backpressure type id");
            if (!tid) {
                return suite.finish();
            }
            slow_type = *tid;
            suite.check(seed(*database, slow_total).has_value(), "backpressure dataset seeded");
            DatabaseRegistry::instance().detach(*attached);
        }

        auto server = Server::listen(slow_path, "127.0.0.1", 0);
        suite.check(server.has_value(), "backpressure server listens");
        if (server) {
            server->use_small_socket_buffers(true);
            const auto port = server->port();
            const auto db_name = std::string{server->database_name()};
            std::thread acceptor([&server, &suite] {
                auto status = server->serve_one();
                suite.check(status.has_value(), "backpressure serve_one completes");
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            {
                auto client = Client::connect("127.0.0.1", port, db_name);
                suite.check(client.has_value(), "backpressure client connects");
                if (client) {
                    suite.check(client->set_recv_buffer_bytes(4 * 1024).has_value(),
                                "client recv buffer shrunk");
                    auto stream = client->query(QueryDescription{.type = slow_type});
                    suite.check(stream.has_value(), "backpressure query starts");
                    if (stream) {
                        std::size_t count = 0;
                        while (true) {
                            auto next = stream->next();
                            if (!next) {
                                suite.check(false, "backpressure stream error");
                                break;
                            }
                            if (!*next) {
                                break;
                            }
                            ++count;
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                        suite.check(static_cast<int>(count) == slow_total,
                                    "slow client received every object");
                    }
                }
            }
            acceptor.join();
            const auto& stats = server->last_stream_stats();
            suite.check(stats.produced == static_cast<std::uint64_t>(slow_total),
                        "produced equals dataset size");
            suite.check(stats.sent == stats.produced, "all produced objects were sent");
            suite.check(stats.max_outstanding <= modb::net::max_in_flight_objects,
                        "produzidos-enviados bounded by max_in_flight_objects");
            suite.check(server->open_snapshot_count() == 0,
                        "no snapshots remain after slow stream");
        }
        cleanup(slow_path);
    }

    // --- 8D: desconexão abrupta libera snapshot ---
    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "disconnect server listens");
        if (server) {
            const auto port = server->port();
            const auto db_name = std::string{server->database_name()};
            std::thread acceptor([&server, &suite] {
                (void)server->serve_one();
                suite.check(server->open_snapshot_count() == 0,
                            "disconnect releases query snapshot");
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            {
                auto client = Client::connect("127.0.0.1", port, db_name);
                suite.check(client.has_value(), "disconnect client connects");
                if (client) {
                    auto stream = client->query(QueryDescription{.type = type_id});
                    suite.check(stream.has_value(), "disconnect query starts");
                    if (stream) {
                        for (int i = 0; i < 5; ++i) {
                            auto next = stream->next();
                            suite.check(next.has_value() && next->has_value(),
                                        "received object before disconnect");
                            if (!next || !*next) {
                                break;
                            }
                        }
                    }
                }
                // Client e ObjectStream destruídos aqui → fecha o socket.
            }
            acceptor.join();
            suite.check(server->open_snapshot_count() == 0,
                        "snapshots cleared after abrupt disconnect");
        }
    }

    // --- NativeSocket: connect recusado ---
    {
        auto listener = NativeSocket::listen("127.0.0.1", 0);
        suite.check(listener.has_value(), "temp listener for refuse test");
        if (listener) {
            auto closed_port = listener->local_port();
            suite.check(static_cast<bool>(listener->close()), "temp listener closes");
            if (closed_port) {
                auto refused = NativeSocket::connect("127.0.0.1", *closed_port);
                suite.check(!refused, "connect to closed port fails");
            }
        }
    }

    // --- 8E: Cancel no meio + conexão reutilizável ---
    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "cancel server listens");
        if (server) {
            const auto port = server->port();
            const auto db_name = std::string{server->database_name()};
            std::thread acceptor([&server, &suite] {
                suite.check(server->serve_one().has_value(), "cancel serve_one completes");
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            {
                auto client = Client::connect("127.0.0.1", port, db_name);
                suite.check(client.has_value(), "cancel client connects");
                if (client) {
                    auto stream = client->query(QueryDescription{.type = type_id});
                    suite.check(stream.has_value(), "cancel query starts");
                    if (stream) {
                        const auto qid = stream->query_id();
                        std::size_t received = 0;
                        bool cancelled = false;
                        while (true) {
                            auto next = stream->next();
                            if (!next) {
                                suite.check(false, "cancel stream error before end");
                                break;
                            }
                            if (!*next) {
                                break;
                            }
                            ++received;
                            if (!cancelled && received >= 10) {
                                suite.check(client->cancel(qid).has_value(),
                                            "cancel message sent");
                                cancelled = true;
                            }
                        }
                        suite.check(received > 0, "cancel received some objects");
                        suite.check(static_cast<int>(received) < total,
                                    "cancel stopped before full dataset");
                    }

                    // Segunda consulta na mesma conexão.
                    auto again = client->query(QueryDescription{.type = type_id, .limit = 5});
                    suite.check(again.has_value(), "second query after cancel starts");
                    if (again) {
                        std::size_t second = 0;
                        while (true) {
                            auto next = again->next();
                            if (!next) {
                                suite.check(false, "second query stream error");
                                break;
                            }
                            if (!*next) {
                                break;
                            }
                            ++second;
                        }
                        suite.check(second >= 1, "connection reusable after cancel");
                    }
                }
            }
            acceptor.join();
        }
    }

    // --- 8E: multiplexação de duas consultas concorrentes ---
    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "multiplex server listens");
        if (server) {
            const auto port = server->port();
            const auto db_name = std::string{server->database_name()};
            std::thread acceptor([&server, &suite] {
                suite.check(server->serve_one().has_value(), "multiplex serve_one completes");
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            {
                auto client = Client::connect("127.0.0.1", port, db_name);
                suite.check(client.has_value(), "multiplex client connects");
                if (client) {
                    constexpr std::uint64_t multiplex_limit = 40;
                    auto stream_a = client->query(
                        QueryDescription{.type = type_id, .limit = multiplex_limit});
                    auto stream_b = client->query(
                        QueryDescription{.type = type_id, .limit = multiplex_limit});
                    suite.check(stream_a.has_value() && stream_b.has_value(),
                                "two concurrent queries start");
                    if (stream_a && stream_b) {
                        suite.check(stream_a->query_id() != stream_b->query_id(),
                                    "concurrent query_ids differ");

                        std::size_t count_a = 0;
                        std::size_t count_b = 0;
                        bool done_a = false;
                        bool done_b = false;
                        while (!done_a || !done_b) {
                            if (!done_a) {
                                auto next = stream_a->next();
                                if (!next) {
                                    suite.check(false, "stream A error");
                                    done_a = true;
                                } else if (!*next) {
                                    done_a = true;
                                } else {
                                    ++count_a;
                                }
                            }
                            if (!done_b) {
                                auto next = stream_b->next();
                                if (!next) {
                                    suite.check(false, "stream B error");
                                    done_b = true;
                                } else if (!*next) {
                                    done_b = true;
                                } else {
                                    ++count_b;
                                }
                            }
                        }
                        suite.check(count_a == multiplex_limit, "stream A received all objects");
                        suite.check(count_b == multiplex_limit, "stream B received all objects");
                    }
                }
            }
            acceptor.join();
        }
    }

    // --- 8E: co_await stream.await_next() ---
    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "await server listens");
        if (server) {
            const auto port = server->port();
            const auto db_name = std::string{server->database_name()};
            std::thread acceptor([&server, &suite] {
                suite.check(server->serve_one().has_value(), "await serve_one completes");
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            {
                auto client = Client::connect("127.0.0.1", port, db_name);
                suite.check(client.has_value(), "await client connects");
                if (client) {
                    auto stream =
                        client->query(QueryDescription{.type = type_id, .limit = 25});
                    suite.check(stream.has_value(), "await query starts");
                    if (stream) {
                        std::vector<DecodedObject> collected;
                        collect_await(*stream, collected);
                        suite.check(!collected.empty(), "co_await collected objects");
                        suite.check(collected.size() == 25, "co_await received limited set");
                    }
                }
            }
            acceptor.join();
        }
    }

    // --- 8F: compressão negociada (rle) no fio ---
    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "compression server listens");
        if (server) {
            const auto port = server->port();
            const auto db_name = std::string{server->database_name()};
            std::thread acceptor([&server, &suite] {
                suite.check(server->serve_one().has_value(), "compression serve_one completes");
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            {
                auto client = Client::connect("127.0.0.1", port, db_name);
                suite.check(client.has_value(), "compression client connects");
                if (client) {
                    suite.check(client->hello_ok().selected_codec == Compression::rle,
                                "session negotiated rle");
                    auto stream =
                        client->query(QueryDescription{.type = type_id, .limit = 16});
                    suite.check(stream.has_value(), "compression query starts");
                    if (stream) {
                        std::size_t count = 0;
                        while (true) {
                            auto next = stream->next();
                            if (!next) {
                                suite.check(false, "compression stream error");
                                break;
                            }
                            if (!*next) {
                                break;
                            }
                            ++count;
                        }
                        suite.check(count == 16, "compression stream delivered objects");
                    }
                }
            }
            acceptor.join();
        }
    }

    // --- 8F: limite de streams concorrentes ---
    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "stream-limit server listens");
        if (server) {
            server->set_max_concurrent_streams(1);
            const auto port = server->port();
            const auto db_name = std::string{server->database_name()};
            std::thread acceptor([&server, &suite] {
                suite.check(server->serve_one().has_value(), "stream-limit serve_one completes");
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            {
                auto client = Client::connect("127.0.0.1", port, db_name);
                suite.check(client.has_value(), "stream-limit client connects");
                if (client) {
                    auto slow = client->query(QueryDescription{.type = type_id});
                    auto overflow = client->query(QueryDescription{.type = type_id, .limit = 1});
                    suite.check(slow.has_value() && overflow.has_value(),
                                "stream-limit both queries sent");
                    if (slow && overflow) {
                        // A segunda deve falhar com StreamError (limite=1).
                        auto overflow_next = overflow->next();
                        suite.check(!overflow_next &&
                                        overflow_next.error().code == ErrorCode::invalid_argument,
                                    "excess concurrent stream rejected");

                        std::size_t count = 0;
                        while (true) {
                            auto next = slow->next();
                            if (!next) {
                                suite.check(false, "primary stream failed after limit reject");
                                break;
                            }
                            if (!*next) {
                                break;
                            }
                            ++count;
                            if (count >= 5) {
                                break;
                            }
                        }
                        suite.check(count >= 1, "primary stream still usable");
                        (void)client->cancel(slow->query_id());
                        while (true) {
                            auto next = slow->next();
                            if (!next || !*next) {
                                break;
                            }
                        }
                    }
                }
            }
            acceptor.join();
        }
    }

    // --- 8F: idle timeout encerra sessão ociosa ---
    {
        auto server = Server::listen(path, "127.0.0.1", 0);
        suite.check(server.has_value(), "timeout server listens");
        if (server) {
            server->set_idle_timeout_ms(150);
            const auto port = server->port();
            const auto db_name = std::string{server->database_name()};
            bool serve_ok = false;
            std::thread acceptor([&server, &serve_ok] {
                serve_ok = static_cast<bool>(server->serve_one());
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            {
                auto client = Client::connect("127.0.0.1", port, db_name);
                suite.check(client.has_value(), "timeout client connects");
                // Sem Query: o servidor deve sair por idle timeout.
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
            acceptor.join();
            suite.check(serve_ok, "idle timeout ends session cleanly");
        }
    }

    cleanup(path);
    return suite.finish();
}
