#include <modb/object/database.hpp>
#include <modb/version.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

// Consumidor externo mínimo (Fase 10E): só headers instalados + lib modb.

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("modb-consumer-" + std::to_string(stamp) + ".modb");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto created = modb::object::Database::create(path);
        if (!created) {
            std::cerr << "create failed: " << created.error().message << '\n';
            return 1;
        }
    }

    auto opened = modb::object::Database::open(path);
    if (!opened) {
        std::cerr << "open failed: " << opened.error().message << '\n';
        return 1;
    }
    std::cout << modb::project_name() << ' ' << modb::project_version()
              << " consumer ok\n";
    std::filesystem::remove(path, ec);
    return 0;
}
