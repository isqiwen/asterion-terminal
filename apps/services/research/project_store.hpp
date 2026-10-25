#pragma once
#include "sqlite_database.hpp"
#include <asterion/research_project.pb.h>
#include <asterion/foundation/time.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <functional>
#include <span>
#include <stop_token>

namespace asterion::tasks {

class Store;

} // namespace asterion::tasks

namespace asterion::research {

// The caller retains the Research root ownership lock and serializes writes
// with the existing task journal. Uses its metadata file, not a second ledger.
// Construct a writer only for an explicit mutation; read-only discovery never
// initializes project tables. No calculation or Data mutation lives here.
class Projects
{
public:
    Projects(
        tasks::Store& store,
        resource::proto::Owner owner,
        sqlite::Database::Access access = sqlite::Database::Access::read_only,
        std::shared_ptr<const Clock> clock = std::make_shared<SystemClock>());
    ~Projects();
    Projects(const Projects&) = delete;
    Projects& operator=(const Projects&) = delete;
    proto::ProjectReply apply(const proto::ProjectMutation&);
    std::optional<proto::ProjectDraft> draft(const std::string&) const;
    proto::HypothesisPlan plan(const std::string&) const;
    std::vector<proto::ProjectDraft> drafts(const std::string& after, unsigned limit) const;
    std::vector<proto::HypothesisPlan>
    plans(const std::string& project, const std::string& after, unsigned limit) const;
    proto::ProjectConclusion conclusion(const std::string&) const;
    std::vector<proto::ProjectConclusion>
    conclusions(const std::string& project, const std::string& after, unsigned limit) const;
    resource::proto::Catalogue resources(const resource::proto::CatalogueQuery&) const;
    void verify_binding(const proto::ProjectBinding&) const;
    // Stopped-domain, read-only metadata and receipt validation. Caller keeps
    // the ownership cut; no project tables or sidecar files are created.
    static void inspect_backup(
        const std::filesystem::path&,
        const resource::proto::Owner&,
        std::span<const proto::ProjectBinding>,
        const std::function<void(const proto::ProjectDraft&)>&,
        const std::function<void(const proto::HypothesisPlan&)>&,
        const std::function<void(const proto::ProjectConclusion&)>&,
        std::stop_token = {});

private:
    Projects(
        sqlite::Database&,
        resource::proto::Owner,
        sqlite::Database::Access,
        std::shared_ptr<const Clock>);
    resource::proto::Owner owner_;
    std::shared_ptr<const Clock> clock_;
    sqlite::Database* database_ = nullptr; // Borrowed; the Store outlives this view.
    bool initialized_ = false, writer_ = false;
};

} // namespace asterion::research
