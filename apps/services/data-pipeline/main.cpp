#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include "history_minutes.hpp"
#include "history_providers.hpp"
#include <CLI/CLI.hpp>
#include <asterion/protocol/task_client.hpp>
#include <asterion/protocol/data_client.hpp>
#include <iostream>
#include <condition_variable>
#include <stdexcept>

namespace {

using namespace asterion;

// Reads what a table update still lacks and hands each partition to the data
// service, which validates and publishes it. Nothing delivered is taken back:
// whatever ends this run, the partitions before it stay.
data::v1::TableUpdateResult update_tables(
    protocol::DataClient& data,
    const data::v1::DownloadIdentity& identity,
    const data::v1::TableUpdate& update,
    history_providers::TableReader& reader,
    std::stop_token stop,
    const std::function<void(unsigned)>& percent)
{
    // Rejections of one kind repeat: stop before they use up the request budget.
    constexpr int rejection_limit = 10, example_limit = 100;
    enum class Outcome
    {
        stored,
        rejected,
        unpublished
    };
    data::v1::TableUpdateResult result;
    std::uint64_t latest = 0;
    std::string reason;
    const auto plan = [&](data::v1::TablePlanStep step) {
        data::v1::DataRequest request;
        *request.mutable_table_plan()->mutable_identity() = identity;
        request.mutable_table_plan()->set_step(step);
        return data.call(request).table_plan();
    };
    const auto deliver = [&](const data::v1::TablePartitionKey& key) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "Historical download cancelled");
        data::v1::DataRequest request;
        auto* delivery = request.mutable_store_table_partition();
        *delivery->mutable_identity() = identity;
        *delivery->mutable_partition() = key;
        try {
            for (auto& cells :
                reader.read(history_table(key.table()), key.exchange(), key.period(), stop)) {
                auto* row = delivery->add_rows();
                for (auto& cell : cells) {
                    auto* value = row->add_cells();
                    if (cell)
                        value->set_text(std::move(*cell));
                }
            }
        } catch (const history_providers::TableRejected& error) {
            reason = error.what();
            return Outcome::rejected;
        }
        if (const auto response = reader.response()) {
            auto* captured = delivery->mutable_response();
            captured->set_format(response->format);
            captured->set_received_at_ns(response->received_at_ns);
            captured->set_body(response->body);
        }
        if (delivery->rows().empty())
            return Outcome::unpublished;
        const auto receipt = data.call(request).table_partition();
        if (receipt.has_rejection()) {
            reason = receipt.rejection();
            return Outcome::rejected;
        }
        if (update.has_refresh()) {
            *result.mutable_refreshed_revision() = receipt.revision();
            if (receipt.has_observation())
                *result.mutable_refresh_observation() = receipt.observation();
        }
        if (receipt.unchanged())
            result.set_unchanged(result.unchanged() + 1);
        else
            result.set_stored(result.stored() + 1);
        if (receipt.revision().sequence() > latest) {
            latest = receipt.revision().sequence();
            result.set_snapshot(receipt.revision().snapshot());
        }
        return Outcome::stored;
    };
    const auto name = [](const data::v1::TablePartitionKey& key) {
        return key.table() + " " + key.exchange() + " " + key.period();
    };
    // The contract lists say when an exchange began, and its calendar from then
    // on which days exist: each is planned once the one before is stored.
    // Without one of them the days of that exchange cannot be planned, so the
    // run ends.
    for (const auto step : {data::v1::TABLE_PLAN_CONTRACTS, data::v1::TABLE_PLAN_CALENDAR}) {
        const auto references = plan(step);
        for (const auto& key : references.partitions())
            switch (deliver(key)) {
                case Outcome::stored:
                    break;
                case Outcome::rejected:
                    throw Error(
                        ErrorCode::operation_failed,
                        "reference data was rejected: " + reason);
                case Outcome::unpublished:
                    throw Error(
                        ErrorCode::operation_failed,
                        "the source returned no reference data: " + name(key));
            }
    }
    const auto days = plan(data::v1::TABLE_PLAN_DAYS);
    for (const auto& waiting : days.waiting())
        result.add_waiting(waiting);
    for (int done = 0; done < days.partitions_size(); ++done) {
        const auto& key = days.partitions(done);
        switch (deliver(key)) {
            case Outcome::stored:
                break;
            case Outcome::rejected: {
                if (update.has_refresh())
                    throw Error(ErrorCode::operation_failed, reason);
                auto* rejected = result.add_rejected();
                *rejected->mutable_partition() = key;
                rejected->set_reason(reason);
                if (result.rejected_size() >= rejection_limit)
                    throw Error(
                        ErrorCode::operation_failed,
                        "too many partitions were rejected; the first: "
                            + result.rejected(0).reason());
                break;
            }
            case Outcome::unpublished:
                result.set_unpublished(result.unpublished() + 1);
                if (result.unpublished_examples_size() < example_limit)
                    *result.add_unpublished_examples() = key;
                break;
        }
        percent(static_cast<unsigned>(100LL * (done + 1) / days.partitions_size()));
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"Asterion historical data downloads from data-source plugins"};
    app.set_version_flag("--version", "asterion-data-pipeline " ASTERION_PRODUCT_VERSION);
    std::string endpoint, host, service, task;
    unsigned short port = 0;
    asterion::ipc::TlsIdentity tls;
    std::uint64_t owner_pid = 0;
    app.add_option("--owner-pid", owner_pid);
    app.add_option("--endpoint", endpoint);
    app.add_option("--host", host);
    app.add_option("--port", port)->check(CLI::Range(1, 65535));
    app.add_option("--tls-ca", tls.ca_file);
    app.add_option("--tls-cert", tls.certificate_file);
    app.add_option("--tls-key", tls.private_key_file);
    app.add_option("--session", service);
    app.add_option("--task", task);
    bool minute_worker = false, table_worker = false;
    app.add_flag("--minute-download", minute_worker, "Run a managed minute-data download");
    app.add_flag("--table-update", table_worker, "Run a managed update of market-wide tables");
    argv = app.ensure_utf8(argv);
    std::string plugin_directory;
    app.add_option("--plugin-directory", plugin_directory)->check(CLI::ExistingDirectory);
    CLI11_PARSE(app, argc, argv);
    try {
        if (!plugin_directory.empty())
            asterion::configure_native_plugins(plugin_directory);
        if (minute_worker + table_worker != 1)
            throw std::invalid_argument(
                "choose exactly one of --minute-download or --table-update");
        {
            return asterion::protocol::run_task_worker(
                endpoint,
                host,
                port,
                tls,
                service,
                task,
                table_worker ? asterion::task::v1::TABLE_UPDATE
                             : asterion::task::v1::MINUTE_DOWNLOAD,
                [table_worker, &service](const auto& attempt, auto stop, const auto& progress) {
                    const auto& input = attempt.task();
                    if (input.provider_artifact()
                        != (table_worker
                                ? asterion::history_providers::table_artifact(
                                      input.table_update().provider())
                                : asterion::history_providers::artifact(input.minutes().source())))
                        throw std::invalid_argument(
                            "download provider artifact does not match task");
                    asterion::protocol::DataClient data(
                        attempt.data_endpoint(),
                        attempt.data_instance());
                    asterion::data::v1::DataRequest query;
                    auto* identity = query.mutable_download_credentials();
                    identity->set_data_instance(attempt.data_instance());
                    identity->set_task_instance(service);
                    identity->set_task_id(input.id());
                    identity->set_attempt(input.attempt());
                    const auto response = data.call(query);
                    const auto& credentials = response.download_credentials();
                    if (credentials.identity().SerializeAsString() != identity->SerializeAsString()
                        || credentials.authorization_id() != input.download_authorization())
                        throw std::invalid_argument("download credentials do not match attempt");
                    asterion::data::v1::DataRequest permit_request;
                    *permit_request.mutable_acquire_download_permit() = *identity;
                    const auto rpm = table_worker ? input.table_update().requests_per_minute()
                                                  : input.minutes().requests_per_minute();
                    auto next_request = std::chrono::steady_clock::now();
                    auto budget = [&](std::stop_token cancelled) {
                        std::mutex mutex;
                        std::condition_variable_any changed;
                        std::unique_lock lock(mutex);
                        changed.wait_until(lock, cancelled, next_request, [] {
                            return false;
                        });
                        while (!cancelled.stop_requested()) {
                            const auto permit = data.call(permit_request, std::chrono::seconds(5))
                                                    .download_permit();
                            if (permit.granted()) {
                                next_request = std::chrono::steady_clock::now()
                                    + std::chrono::milliseconds((60000 + rpm - 1) / rpm);
                                return;
                            }
                            if (!permit.retry_after_ms() || permit.retry_after_ms() > 60000)
                                throw std::invalid_argument("invalid download request permit");
                            changed.wait_for(
                                lock,
                                cancelled,
                                std::chrono::milliseconds(permit.retry_after_ms()),
                                [] {
                                    return false;
                                });
                        }
                        throw asterion::Error(
                            asterion::ErrorCode::cancelled,
                            "Historical download cancelled");
                    };
                    asterion::task::v1::TaskFinish result;
                    if (table_worker) {
                        auto reader = asterion::history_providers::tables(
                            input.table_update().provider(),
                            credentials.credential(),
                            budget);
                        *result.mutable_table_update() = update_tables(
                            data,
                            *identity,
                            input.table_update(),
                            *reader,
                            stop,
                            [&](unsigned percent) {
                                progress(percent, 100);
                            });
                        return result;
                    }
                    const auto directory = std::filesystem::path(
                        std::u8string(
                            attempt.output_directory().begin(),
                            attempt.output_directory().end()));
                    auto provider = asterion::history_providers::minutes(
                        input.minutes().source(),
                        credentials.credential(),
                        budget);
                    (void)asterion::history_files::download_minutes(
                        *provider,
                        asterion::history_files::minute_range(input.minutes()),
                        directory,
                        stop,
                        [&](unsigned value, unsigned total, std::uint64_t) {
                            progress(value, total);
                        });
                    *result.mutable_minutes() = asterion::history_files::minute_result(directory);
                    return result;
                },
                owner_pid);
        }
    } catch (const std::exception& error) {
        std::cerr << "Data download failed: " << error.what() << '\n';
        asterion::log_process_event(
            "data-pipeline",
            asterion::LogLevel::error,
            "service.failed",
            {{"message", error.what()}});
        return 1;
    }
}
