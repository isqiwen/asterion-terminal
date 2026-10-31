#include "acquisition_tasks.hpp"
#include "data_submission.hpp"
#include "acquisition_execution.hpp"
#include "material_providers.hpp"
#include <asterion/system/process/child.hpp>
#include <asterion/system/rpc_host.hpp>

namespace asterion::data {

using namespace std::chrono_literals;

AcquisitionTasks::AcquisitionTasks(
    std::filesystem::path directory,
    std::string service,
    std::string endpoint,
    Store& data,
    std::string node,
    Materials* materials)
    : data_(data),
      service_(std::move(service)),
      endpoint_(std::move(endpoint)),
      node_(std::move(node)),
      program_directory_(std::filesystem::absolute(directory).parent_path() / "programs"),
      materials_(materials),
      tasks_(task_domain(), std::move(directory), {service_})
{}

proto::AcquisitionTaskResponse AcquisitionTasks::handle(
    const proto::AcquisitionTaskRequest& request)
{
    proto::AcquisitionTaskResponse response;
    if (request.has_material_submit()) {
        if (!materials_ || node_.empty())
            throw Error(ErrorCode::unavailable, "material acquisition requires stable Data owner");
        const auto& requested = request.material_submit();
        protocol::validate_material_request(requested);
        proto::MaterialInput fixed;
        *fixed.mutable_request() = requested;
        auto& owner = *fixed.mutable_owner();
        owner.set_node_id(node_);
        owner.set_service_id(service_);
        owner.set_domain("data");
        std::optional<task::proto::Task> previous;
        if (tasks_.find_task(requested.id()))
            previous = tasks_.get(requested.id());
        if (previous) {
            if (!previous->has_material() || previous->has_retirement()
                || previous->material().SerializeAsString() != fixed.SerializeAsString())
                throw Error(
                    ErrorCode::conflict,
                    "material task identity has different input or is retired");
            *response.mutable_task() = std::move(*previous);
            return response;
        }
        const auto sources = material_providers::sources();
        const auto selected = std::find_if(sources.begin(), sources.end(), [&](const auto& s) {
            return s.id == requested.source_id();
        });
        if (selected == sources.end())
            throw Error(ErrorCode::unavailable, "material source is not installed");
        if (selected->artifact != requested.provider_artifact())
            throw Error(ErrorCode::conflict, "material provider differs from reviewed plan");
        if (selected->credential_required || !selected->no_additional_fee)
            throw Error(
                ErrorCode::permission_denied,
                "material source access or cost is unsupported");
        if (requested.max_bytes() > selected->max_bytes)
            throw std::invalid_argument("material response limit exceeds source capability");
        material_providers::preserve(requested, program_directory_);
        auto submission = task_submission(tasks_, std::move(fixed));
        tasks_.admit_submission(submission);
        try {
            submission.prepare_files();
            *response.mutable_task() = tasks_.commit(tasks_.register_submission(submission)).task();
        } catch (...) {
            if (tasks_.abandon_submission(submission)) {
                tasks_.persistence_failed();
                throw Error(ErrorCode::operation_failed, "material submission requires recovery");
            }
            throw;
        }
    } else if (request.has_submit()) {
        const auto authorization = data_.authorization(request.submit().authorization());
        if (authorization.task_id() != request.submit().id()
            || authorization.task_instance() != service_
            || (!authorization.has_table_update() && !authorization.has_minutes()))
            throw Error(ErrorCode::conflict, "acquisition belongs to the data service");
        auto submission = task_submission(tasks_, authorization);
        tasks_.admit_submission(submission);
        try {
            submission.prepare_files();
            *response.mutable_task() = tasks_.commit(tasks_.register_submission(submission)).task();
        } catch (...) {
            if (tasks_.abandon_submission(submission)) {
                tasks_.persistence_failed();
                throw Error(ErrorCode::operation_failed, "task submission requires recovery");
            }
            throw;
        }
    } else if (request.has_get()) {
        *response.mutable_task() = tasks_.get(request.get().id());
    } else if (request.has_list()) {
        *response.mutable_tasks() =
            tasks_.list(request.list().limit(), request.list().before_sequence());
    } else if (request.has_cancel()) {
        *response.mutable_task() = tasks_.commit(tasks_.cancel(request.cancel().id())).task();
    } else if (request.has_retry()) {
        *response.mutable_task() = tasks_.commit(tasks_.retry(request.retry().id())).task();
    } else if (request.has_metadata_edit()) {
        *response.mutable_task() =
            tasks_.commit(tasks_.edit_metadata(request.metadata_edit())).task();
    } else if (request.has_retirement()) {
        if (node_.empty())
            throw Error(
                ErrorCode::unavailable,
                "acquisition retirement requires Agent node identity");
        auto preview = tasks_.retirement(request.retirement().id(), node_);
        if (request.retirement().execute()) {
            tasks_.commit(tasks_.retire(preview.object()));
            preview = tasks_.retirement(request.retirement().id(), node_);
        }
        *response.mutable_retirement() = std::move(preview);
    } else if (request.has_active_history_usage()) {
        *response.mutable_history_usage() =
            tasks_.history_usage(request.active_history_usage().id(), true);
    } else if (request.has_history_usage()) {
        *response.mutable_history_usage() = tasks_.history_usage(request.history_usage().id());
    } else if (request.has_result()) {
        auto read = tasks_.prepare_result(request.result().id());
        read.verify();
        const auto verified = tasks_.confirm_result(std::move(read));
        if (verified.has_table_update())
            *response.mutable_table_update() = verified.table_update();
        if (verified.has_minutes())
            *response.mutable_minutes() = verified.minutes();
        if (verified.has_material())
            *response.mutable_material() = verified.material();
        *response.mutable_task() = verified.result_task();
    } else
        throw std::invalid_argument("missing data refresh operation");
    return response;
}

std::vector<std::string> AcquisitionTasks::snapshot_holds(const std::string& snapshot)
{
    auto result = table_snapshot_holds(tasks_, snapshot);
    for (const auto& observer : data_.tables().snapshot_observers(snapshot)) {
        if (observer.data_instance != service_ || observer.task_instance != service_)
            throw Error(
                ErrorCode::unavailable,
                "snapshot publication has an unknown acquisition owner");
        const auto task = tasks_.describe(observer.task_id);
        if (task.kind() != task::proto::TABLE_UPDATE)
            throw Error(ErrorCode::conflict, "snapshot publication task kind mismatch");
        if (!task.has_retirement())
            result.push_back(task.id());
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    if (result.size() > 10000)
        throw Error(ErrorCode::resource_exhausted, "snapshot acquisition holds exceed capacity");
    return result;
}

bool AcquisitionTasks::advance(bool admitting, bool stopping)
{
    for (auto& execution : executions_) {
        if (!execution.future.valid())
            continue;
        const auto current = tasks_.describe(execution.id);
        if (stopping || current.state() == task::proto::CANCEL_REQUESTED)
            execution.cancel.request_stop();
        if (execution.future.wait_for(0ms) != std::future_status::ready) {
            const auto progress = execution.progress.load();
            if (current.state() == task::proto::RUNNING && progress > current.completed())
                tasks_.commit(tasks_.progress(execution.id, execution.token, progress));
            continue;
        }
        std::exception_ptr failure;
        task::proto::TaskFinish result;
        try {
            execution.future.get();
            result = std::move(execution.result);
        } catch (...) {
            failure = std::current_exception();
        }
        const auto state = tasks_.describe(execution.id).state();
        if (state == task::proto::CANCEL_REQUESTED) {
            tasks_.commit(tasks_.acknowledge_cancel(execution.id, execution.token));
        } else if (failure) {
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& error) {
                tasks_.commit(
                    stopping ? tasks_.interrupt(execution.id, execution.token, error.what())
                             : tasks_.fail(
                                   execution.id,
                                   execution.token,
                                   error.what(),
                                   std::string(error_name(classify(error)))));
            }
        } else {
            if (execution.material) {
                try {
                    *result.mutable_material() = materials_->publish(*execution.material);
                } catch (const std::exception& error) {
                    tasks_.commit(tasks_.fail(
                        execution.id,
                        execution.token,
                        error.what(),
                        std::string(error_name(classify(error)))));
                    execution.material.reset();
                    execution.id.clear();
                    execution.token.clear();
                    continue;
                }
            }
            result.set_id(execution.id);
            result.set_token(execution.token);
            auto completion = tasks_.prepare_finish(result);
            completion.prepare_payload();
            if (result.has_minutes()) {
                const auto verified =
                    data_.verify_download(download_preparation(completion, tasks_.instance()));
                const auto prepared = data_.prepare(verified);
                tasks_.commit(prepare_publication(tasks_, std::move(completion), prepared));
            } else
                tasks_.commit(tasks_.finish(std::move(completion)));
        }
        execution.material.reset();
        execution.id.clear();
        execution.token.clear();
    }
    // Only already durable publication decisions resume after a restart; no provider retry.
    for (const auto& decision : tasks_.pending_publications()) {
        const auto verified = data_.verify_publication(decision);
        const auto published = data_.publish(verified);
        tasks_.commit(confirm_publication(tasks_, published));
    }
    if (!admitting || stopping)
        return std::none_of(executions_.begin(), executions_.end(), [](const auto& execution) {
            return execution.future.valid();
        });
    for (const auto& task : tasks_.active_tasks()) {
        if (task.state() != task::proto::QUEUED)
            continue;
        auto available =
            std::find_if(executions_.begin(), executions_.end(), [](const auto& execution) {
                return !execution.future.valid();
            });
        if (available == executions_.end())
            break;
        auto& execution = *available;
        execution.id = task.id();
        try {
            const auto input = tasks_.get(task.id());
            auto read = tasks_.prepare_input(task.id());
            read.load_for_claim();
            auto claim = tasks_.commit(tasks_.claim(read));
            execution.id = task.id();
            execution.token = claim.token();
            auto attempt = tasks_.confirm_attempt(std::move(read));
            attempt.set_token(execution.token);
            attempt.set_data_endpoint(endpoint_);
            attempt.set_data_instance(service_);
            execution.material.reset();
            if (input.has_material()) {
                if (!materials_)
                    throw Error(ErrorCode::unavailable, "material store unavailable");
                if (input.material().owner().node_id() != node_
                    || input.material().owner().service_id() != service_)
                    throw Error(
                        ErrorCode::conflict,
                        "material task owner differs from this Data service");
                execution.cancel = std::stop_source();
                execution.progress = 0;
                execution.future = provider_.submit(
                    [this, &execution, fixed = input.material()](std::stop_token closing) {
                        std::stop_callback cancel(closing, [&execution] {
                            execution.cancel.request_stop();
                        });
                        const auto stop = execution.cancel.get_token();
                        std::optional<Materials::Read> prior;
                        try {
                            prior = materials_->read(fixed.request().id());
                        } catch (const Error& error) {
                            if (error.code() != ErrorCode::not_found)
                                throw;
                        }
                        if (prior) {
                            protocol::validate_material_result(fixed, prior->receipt);
                            *execution.result.mutable_material() = std::move(prior->receipt);
                        } else {
                            const auto response = material_providers::fetch(
                                fixed.request(),
                                [](std::stop_token stop) {
                                    if (stop.stop_requested())
                                        throw Error(
                                            ErrorCode::cancelled,
                                            "material acquisition cancelled");
                                },
                                stop);
                            execution.material = materials_->prepare(
                                fixed.request(),
                                response.bytes,
                                response.media_type,
                                response.acquired_at_ms,
                                response.source_published_at_ms);
                        }
                        execution.progress = 1;
                    });
                continue;
            }
            proto::DownloadAllocation allocation;
            allocation.set_authorization_id(input.download_authorization());
            auto* identity = allocation.mutable_identity();
            identity->set_data_instance(service_);
            identity->set_task_instance(service_);
            identity->set_task_id(execution.id);
            identity->set_attempt(attempt.task().attempt());
            if (input.has_minutes())
                *allocation.mutable_minutes() = input.minutes();
            else
                *allocation.mutable_table_update() = input.table_update();
            auto verified = data_.verify_allocation(allocation);
            attempt.set_output_directory(data_.allocate(verified).directory());
            execution.cancel = std::stop_source();
            execution.progress = 0;
            execution.future = provider_.submit(
                [this, &execution, attempt = std::move(attempt)](std::stop_token stop) {
                    std::stop_callback closing(stop, [&execution] {
                        execution.cancel.request_stop();
                    });
                    execution.result = download(
                        attempt,
                        service_,
                        execution.cancel.get_token(),
                        [&execution](unsigned completed, unsigned) {
                            execution.progress = completed;
                        });
                });
        } catch (const std::exception& error) {
            if (execution.token.empty())
                tasks_.commit(tasks_.fail_start(
                    execution.id,
                    task.attempt() + 1,
                    error.what(),
                    std::string(error_name(classify(error)))));
            else
                tasks_.commit(tasks_.fail(
                    execution.id,
                    execution.token,
                    error.what(),
                    std::string(error_name(classify(error)))));
            execution.id.clear();
            execution.token.clear();
        }
    }
    return std::none_of(executions_.begin(), executions_.end(), [](const auto& execution) {
        return execution.future.valid();
    });
}

} // namespace asterion::data
