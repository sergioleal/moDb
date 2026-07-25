// Lesson 16 -- Blobs, Sets, and Maps.
// Builds on Lesson 15 by returning to the SAME employee-directory database
// and adding a durable TrainingArtifact used by Lesson 17.
// See docs/training/en/16-blobs-sets-and-maps/16-blobs-sets-and-maps.md.

#include "modb/object/collection.hpp"
#include "modb/object/database.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using modb::object::AttributeValue;
using modb::object::BlobId;
using modb::object::FieldId;

namespace {

struct TrainingArtifact {
    std::string name;
    BlobId note{};
    BlobId tags{};
    BlobId scores{};
};

modb::object::BindingBuilder<TrainingArtifact> artifact_binding() {
    modb::object::BindingBuilder<TrainingArtifact> builder{"TrainingArtifact"};
    builder.field<1>("name", &TrainingArtifact::name)
        .field<2>("note", &TrainingArtifact::note, BlobId{})
        .field<3>("tags", &TrainingArtifact::tags, BlobId{})
        .field<4>("scores", &TrainingArtifact::scores, BlobId{});
    return builder;
}

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return out;
}

} // namespace

int main() {
    std::cout << "Objective: add blob-backed training artifacts to the directory database.\n";

    auto opened = modb::object::Database::open(db_path());
    if (!opened) {
        std::cerr << opened.error().message << " -- have you run Lessons 1-15 first?\n";
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached || !database->bind(artifact_binding())) {
        std::cerr << "failed to bind TrainingArtifact\n";
        return 1;
    }

    constexpr FieldId name_field{1};
    auto existing = database->indexed_object_ids<TrainingArtifact>(
        name_field, AttributeValue{std::string{"ops-artifact"}});
    if (existing && !existing->empty()) {
        std::cout << "TrainingArtifact ops-artifact already exists as object "
                  << existing->front().value << '\n';
    } else {
        auto tx = database->begin();
        if (!tx) {
            std::cerr << tx.error().message << '\n';
            return 1;
        }
        auto blobs = database->blobs();

        auto note = blobs.create(bytes("Operational notes: diagnose first, then repair."));
        auto tags = modb::object::PersistentSet<std::string>::create(blobs, *tx);
        auto scores = modb::object::PersistentMap<std::string, std::int64_t>::create(blobs, *tx);
        if (!note || !tags || !scores) {
            std::cerr << "failed to create blob-backed structures\n";
            return 1;
        }
        (void)tags->insert(*tx, "cli");
        (void)tags->insert(*tx, "diagnostics");
        (void)tags->insert(*tx, "replication");
        (void)tags->insert(*tx, "cli");
        (void)scores->put(*tx, "diagnostics", 90);
        (void)scores->put(*tx, "diagnostics", 95);
        (void)scores->put(*tx, "replication", 85);

        auto created = database->create(
            *tx, TrainingArtifact{"ops-artifact", *note, tags->id(), scores->id()});
        if (!created) {
            std::cerr << created.error().message << '\n';
            return 1;
        }
        if (auto committed = tx->commit(); !committed) {
            std::cerr << committed.error().message << '\n';
            return 1;
        }
        if (auto indexed = database->create_index<TrainingArtifact>(name_field); !indexed) {
            std::cerr << indexed.error().message << '\n';
            return 1;
        }
        std::cout << "Created TrainingArtifact ops-artifact as object " << created->id().value
                  << '\n';
    }

    auto matches = database->indexed_object_ids<TrainingArtifact>(
        name_field, AttributeValue{std::string{"ops-artifact"}});
    if (!matches || matches->empty()) {
        std::cerr << "could not find ops-artifact by index\n";
        return 1;
    }
    auto handle = database->get<TrainingArtifact>(matches->front());
    if (!handle) {
        std::cerr << handle.error().message << '\n';
        return 1;
    }
    auto artifact = database->materialize(*handle);
    if (!artifact) {
        std::cerr << artifact.error().message << '\n';
        return 1;
    }

    auto blobs = database->blobs();
    auto note = blobs.read(artifact->note);
    modb::object::PersistentSet<std::string> tags{blobs, artifact->tags};
    modb::object::PersistentMap<std::string, std::int64_t> scores{blobs, artifact->scores};
    auto tag_count = tags.size();
    auto diagnostics_score = scores.get("diagnostics");
    if (!note || !tag_count || !diagnostics_score || !*diagnostics_score) {
        std::cerr << "failed to read blob-backed artifact\n";
        return 1;
    }

    std::cout << "Note bytes: " << note->size() << '\n';
    std::cout << "Tags stored: " << *tag_count << " (after duplicate inserts)\n";
    std::cout << "Score for diagnostics: " << **diagnostics_score << '\n';

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return 0;
}
