#pragma once
#include <asterion/database/sqlite/sqlite_database.hpp>
#include <asterion/research_logic.pb.h>
#include <asterion/foundation/time.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace asterion::tasks {

class Store;

} // namespace asterion::tasks

namespace asterion::research {

// Borrowed Research connection, serialized by its existing journal and root owner.
class Logic
{
public:
    Logic(
        tasks::Store&,
        resource::proto::Owner,
        sqlite::Database::Access = sqlite::Database::Access::read_only,
        std::shared_ptr<const Clock> = std::make_shared<SystemClock>());
    std::optional<proto::LogicReply> receipt(const proto::LogicMutation&) const;
    proto::LogicReply apply(const proto::LogicMutation&);
    proto::LogicDefinition definition(const std::string&) const;
    proto::LogicExample example(const std::string&) const;
    std::vector<proto::LogicDefinition> definitions(const std::string& after, unsigned limit) const;
    std::vector<proto::LogicExample>
    examples(const std::string& definition, const std::string& after, unsigned limit) const;
    proto::LogicTrialInput input(const resource::proto::Object& example, bool admission) const;
    void verify_input(const proto::LogicTrialInput&, bool admission) const;
    resource::proto::Catalogue resources(const resource::proto::CatalogueQuery&) const;
    static void inspect_backup(
        const std::filesystem::path&,
        const resource::proto::Owner&,
        std::span<const proto::LogicTrialInput>,
        const std::function<void(const proto::LogicDefinition&)>&,
        const std::function<void(const proto::LogicExample&)>&,
        std::stop_token = {});

private:
    friend class Strategies;
    Logic(
        sqlite::Database&,
        resource::proto::Owner,
        sqlite::Database::Access,
        std::shared_ptr<const Clock>);
    sqlite::Database* database_;
    resource::proto::Owner owner_;
    bool writer_;
    std::shared_ptr<const Clock> clock_;
};

} // namespace asterion::research
