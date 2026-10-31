#include "data_submission.hpp"
#include <asterion/base/error.hpp>
#include <asterion/base/id.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/material.hpp>

namespace asterion::tasks {
namespace wire = task::proto;

Store::Submission task_submission(const Store& store, data::proto::MaterialInput input)
{
    protocol::validate_material_input(input);
    if (input.owner().service_id() != store.instance())
        throw Error(ErrorCode::conflict, "material acquisition belongs to another Data owner");
    wire::Task task;
    task.set_id(input.request().id());
    *task.mutable_material() = std::move(input);
    return store.submission(std::move(task));
}

Store::Submission
task_submission(const Store& store, const data::proto::DownloadAuthorization& authorization)
{
    protocol::validate_message(authorization);
    validate_id(authorization.task_id());
    if (authorization.data_instance() != store.instance()
        || authorization.task_instance() != store.instance() || authorization.id().size() != 64
        || authorization.id().find_first_not_of("0123456789abcdef") != std::string::npos
        || authorization.provider_artifact().size() != 64
        || authorization.provider_artifact().find_first_not_of("0123456789abcdef")
            != std::string::npos)
        throw std::invalid_argument("invalid task download authorization");
    wire::Task task;
    task.set_id(authorization.task_id());
    task.set_download_authorization(authorization.id());
    task.set_provider_artifact(authorization.provider_artifact());
    if (authorization.has_minutes())
        *task.mutable_minutes() = authorization.minutes();
    else if (authorization.has_table_update())
        *task.mutable_table_update() = authorization.table_update();
    else
        throw std::invalid_argument("download definition is missing");
    return store.submission(std::move(task));
}

} // namespace asterion::tasks
