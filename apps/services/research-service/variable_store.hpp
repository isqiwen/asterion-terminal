#pragma once
#include "sqlite_database.hpp"
#include <asterion/research_variable.pb.h>
#include <asterion/foundation/time.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

namespace asterion::tasks {

class Store;

} // namespace asterion::tasks

namespace asterion::research {

// Research-owned immutable definitions share its original locked metadata and
// journal. No separate database connection, task framework or Data mutation.
class Variables
{
public:
    Variables(
        tasks::Store&,
        resource::proto::Owner,
        sqlite::Database::Access = sqlite::Database::Access::read_only,
        std::shared_ptr<const Clock> = std::make_shared<SystemClock>());
    static proto::VariablePreview preview(const proto::VariableRegister&);
    proto::VariableReply apply(const proto::VariableMutation&);
    proto::VariableDefinition definition(const std::string&) const;
    std::vector<proto::VariableDefinition> list(const std::string& after, unsigned limit) const;
    void verify(const resource::proto::Object&) const;
    resource::proto::Catalogue resources(const resource::proto::CatalogueQuery&) const;
    static void inspect_backup(
        const std::filesystem::path&,
        const resource::proto::Owner&,
        std::span<const resource::proto::Object>,
        const std::function<void(const proto::VariableDefinition&)>&,
        std::stop_token = {});

private:
    Variables(
        sqlite::Database&,
        resource::proto::Owner,
        sqlite::Database::Access,
        std::shared_ptr<const Clock>);
    resource::proto::Owner owner_;
    std::shared_ptr<const Clock> clock_;
    sqlite::Database* database_;
    bool writer_;
};

} // namespace asterion::research
