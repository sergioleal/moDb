// Lesson 17 -- Catalog and Baselines.
// Builds on Lesson 16 by evolving TrainingArtifact and checking that the
// previous baseline remains available.
// See docs/training/en/17-catalog-and-baselines/17-catalog-and-baselines.md.

#include "modb/object/database.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

using modb::object::AttributeValue;
using modb::object::BlobId;
using modb::object::FieldId;

namespace {

struct TrainingArtifactV2 {
    std::string name;
    BlobId note{};
    BlobId tags{};
    BlobId scores{};
    std::string owner;
};

modb::object::BindingBuilder<TrainingArtifactV2> artifact_v2_binding() {
    modb::object::BindingBuilder<TrainingArtifactV2> builder{"TrainingArtifact"};
    builder.field<1>("name", &TrainingArtifactV2::name)
        .field<2>("note", &TrainingArtifactV2::note, BlobId{})
        .field<3>("tags", &TrainingArtifactV2::tags, BlobId{})
        .field<4>("scores", &TrainingArtifactV2::scores, BlobId{})
        .field<5>("owner", &TrainingArtifactV2::owner, "platform-team");
    return builder;
}

std::filesystem::path db_path() {
    return std::filesystem::path{MODB_TRAINING_DIR} / "employee-directory.modb";
}

} // namespace

int main() {
    std::cout << "Objective: evolve a training artifact and inspect catalog baselines.\n";

    auto opened = modb::object::Database::open(db_path());
    if (!opened) {
        std::cerr << opened.error().message << " -- have you run Lesson 16 first?\n";
        return 1;
    }
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    if (!attached) {
        std::cerr << "failed to attach database\n";
        return 1;
    }

    const auto before = database->current_baseline()
                            ? database->current_baseline()->id()
                            : modb::object::BaselineId{};
    std::cout << "Baseline before TrainingArtifactV2: " << before.value << '\n';

    if (!database->bind(artifact_v2_binding())) {
        std::cerr << "failed to bind TrainingArtifactV2\n";
        return 1;
    }

    const auto after = database->current_baseline()
                           ? database->current_baseline()->id()
                           : modb::object::BaselineId{};
    std::cout << "Baseline after TrainingArtifactV2: " << after.value << '\n';

    auto historical = database->find_baseline(before);
    std::cout << "Historical baseline still loadable: " << (historical ? "yes" : "no") << '\n';

    auto matches = database->indexed_object_ids<TrainingArtifactV2>(
        FieldId{1}, AttributeValue{std::string{"ops-artifact"}});
    if (!matches || matches->empty()) {
        std::cerr << "could not find ops-artifact; run Lesson 16 first\n";
        return 1;
    }
    auto handle = database->get<TrainingArtifactV2>(matches->front());
    if (!handle) {
        std::cerr << handle.error().message << '\n';
        return 1;
    }
    auto artifact = database->materialize(*handle);
    if (!artifact) {
        std::cerr << artifact.error().message << '\n';
        return 1;
    }
    std::cout << "ops-artifact owner through V2 binding: " << artifact->owner << '\n';

    modb::object::DatabaseRegistry::instance().detach(*attached);
    return historical ? 0 : 1;
}
