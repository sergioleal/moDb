#include "environments.hpp"

#include "json_value.hpp"

#include <fstream>
#include <sstream>

namespace modb::loadtest {

const EnvironmentEntry* EnvironmentCatalog::find(std::string_view id) const {
    for (const auto& e : environments) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

LoadCatalogResult load_environment_catalog(const std::filesystem::path& path) {
    LoadCatalogResult result;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = "não foi possível abrir " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    auto parsed = parse_json(text);
    if (!parsed.ok) {
        result.error = path.string() + ":" + parsed.error;
        return result;
    }
    if (!parsed.value.is_object()) {
        result.error = path.string() + ": documento raiz precisa ser um objeto";
        return result;
    }

    const auto* schema = parsed.value.find("schema");
    if (!schema || !schema->is_string() || schema->as_string() != "modb.loadtest.environments") {
        result.error = path.string() + ": campo 'schema' ausente ou diferente de "
                                       "'modb.loadtest.environments'";
        return result;
    }

    const auto* envs = parsed.value.find("environments");
    if (!envs || !envs->is_array()) {
        result.error = path.string() + ": campo 'environments' ausente ou não é lista";
        return result;
    }

    for (const auto& item : envs->as_array()) {
        if (!item.is_object()) {
            result.error = path.string() + ": item de 'environments' não é objeto";
            return result;
        }
        EnvironmentEntry entry;
        entry.id = item.get_string("id");
        entry.label = item.get_string("label");
        entry.kind = item.get_string("kind");
        entry.host_class = item.get_string("host_class");
        entry.os_hint = item.get_string("os_hint");
        entry.notes = item.get_string("notes");

        if (entry.id.empty()) {
            result.error = path.string() + ": ambiente sem 'id'";
            return result;
        }
        if (entry.kind != "local" && entry.kind != "ssh") {
            result.error = path.string() + ": ambiente '" + entry.id +
                           "' tem kind inválido (esperado 'local' ou 'ssh'): '" + entry.kind + "'";
            return result;
        }
        if (entry.host_class.empty()) {
            result.error = path.string() + ": ambiente '" + entry.id + "' sem 'host_class'";
            return result;
        }
        if (const auto* dep = item.find("deprecated"); dep && dep->is_bool()) {
            entry.deprecated = dep->as_bool();
        }

        if (entry.kind == "ssh") {
            const auto* conn = item.find("connection");
            if (!conn || !conn->is_object()) {
                result.error =
                    path.string() + ": ambiente '" + entry.id + "' é kind=ssh mas não tem 'connection'";
                return result;
            }
            EnvironmentConnection connection;
            connection.host = conn->get_string("host");
            connection.default_user = conn->get_string("default_user");
            connection.remote_work_dir = conn->get_string("remote_work_dir");
            connection.binary_name = conn->get_string("binary_name");
            if (connection.host.empty()) {
                result.error =
                    path.string() + ": ambiente '" + entry.id + "' sem 'connection.host'";
                return result;
            }
            entry.connection = std::move(connection);
        }

        result.catalog.environments.push_back(std::move(entry));
    }

    result.ok = true;
    return result;
}

ValidateEnvironmentResult validate_environment(const EnvironmentCatalog& catalog,
                                               std::string_view environment_id, bool local_only) {
    ValidateEnvironmentResult result;
    const auto* entry = catalog.find(environment_id);
    if (!entry) {
        std::string known;
        for (const auto& e : catalog.environments) {
            if (!known.empty()) {
                known += ", ";
            }
            known += e.id;
        }
        result.error = "ambiente '" + std::string{environment_id} +
                       "' não está cadastrado. Conhecidos: " + known;
        return result;
    }
    if (local_only && entry->kind == "ssh") {
        result.warning = "ambiente '" + std::string{environment_id} +
                         "' é kind='ssh', mas esta execução é local -- host_class será registrado "
                         "mesmo assim, sem despachar nada por SSH.";
    }
    result.ok = true;
    return result;
}

} // namespace modb::loadtest
