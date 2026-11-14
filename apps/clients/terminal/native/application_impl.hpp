#pragma once
// Internal to the Terminal application: shared state of Application and the
// per-domain command registrations (commands_*.cpp). Not a public API.
#include "terminal_application.hpp"
#include <asterion/futures/account.hpp>
#include "warehouse_client.hpp"
#include "market_client.hpp"
#include "lab_supervisor.hpp"
#include "program_paths.hpp"
#include "trading_connections.hpp"
#include "saved_endpoint.hpp"
#include "python_runtime.hpp"
#include "application_environment.hpp"
#include <asterion/system/environment.hpp>
#include "trading_client.hpp"
#include "service_io.hpp"
#include <asterion/futures/futures.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/trace.hpp>
#include <asterion/base/time.hpp>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <map>
#include <optional>

namespace asterion::terminal {

using nlohmann::json;
using namespace asterion;
void fields(const json& object, std::initializer_list<std::string_view> names);
std::string text(const json& object, const char* name, bool allow_empty = false);
unsigned short port_number(const json& p, const char* name);

struct Application::Impl
{
    struct Publication;

    // A command fixes its publication on the owner. Queues retain references and
    // query-specific values; full JSON exists only while a response worker uses it.
    struct Response
    {
        Response(json value)
            : values(std::move(value))
        {}

        explicit Response(std::shared_ptr<const Publication> state)
            : publication(std::move(state)),
              values(json::object())
        {}

        json render() &&;

        std::shared_ptr<const Publication> publication;
        json values;
    };

    struct ResetFlag
    {
        ~ResetFlag()
        {
            flag = false;
        }

        bool& flag;
    };

    // UI commands belong to this Native boundary; registration is constructor-only.
    using Command = std::function<PolledTask<Response>(const json&)>;

    // Settings files are used by one admitted management job.
    // Only the returned display projection is applied to the I/O owner's state.
    struct SettingsView
    {
        bool operator==(const SettingsView&) const = default;

        json trading = json::array();
        std::string client_fingerprint;
        json market_endpoint = nullptr;
        json warehouse_endpoint = nullptr;
    };

    using Settings = std::shared_ptr<const SettingsView>;

    // The owner fixes these references and their revision before any rendering.
    struct Publication
    {
        bool same_state(const Publication&) const;
        json render() const;

        json metadata;
        json lab;
        Settings settings;
        std::uint64_t revision = 0;
        std::int64_t refreshed_at_ms = 0;
    };

    void record_refresh_failure(ErrorCode code) noexcept;
    // A saved address for display, or a mark that it is unreadable.
    json endpoint_settings(const SavedEndpoint&) const;
    PolledTask<ServiceEndpoint> address_of(const SavedEndpoint&, const char* name);

    // The development entry names the runtime it prepared; an installed
    // application uses the one installed at its first launch.
    std::optional<std::filesystem::path> python() const
    {
        if (auto prepared = environment_path("ASTERION_PYTHON"))
            return prepared;
        return python_runtime.python();
    }

    void command(std::string name, Command handler);
    PolledTask<Response> invoke(const std::string& method, const json& params);

    // Coroutine factories retain their captures until their child operation finishes.
    template <class T, class F>
    static PolledTask<T> run(F work)
    {
        co_return co_await work();
    }

    // One Market connection is opened at a time.
    template <class T, class F>
    PolledTask<T> connect(F work)
    {
        if (connecting)
            throw Error(
                ErrorCode::conflict,
                "Market is already connecting; retry after it completes");
        connecting = true;
        ResetFlag reset{connecting};
        co_return co_await work();
    }

    template <class T, class F>
    PolledTask<T> settings(F work)
    {
        co_await PollUntil{[this] {
            return !settings_busy;
        }};
        settings_busy = true;
        ResetFlag reset{settings_busy};
        if constexpr (std::is_void_v<T>) {
            auto next = co_await service_io.admin<Settings>([&, previous = settings_view] {
                work();
                return inspect_settings(previous);
            });
            settings_view = std::move(next);
        } else {
            auto result =
                co_await service_io.admin<std::pair<T, Settings>>([&, previous = settings_view] {
                    auto value = work();
                    return std::pair{std::move(value), inspect_settings(previous)};
                });
            settings_view = std::move(result.second);
            co_return std::move(result.first);
        }
    }

    Settings inspect_settings(const Settings& previous) const;
    Impl();
    ~Impl();
    Publication capture();
    void publish();
    Response snapshot();
    Response read_published(const json& params);
    PolledTask<void> refresh_loop(std::stop_token stop);
    PolledTask<void> settings_loop(std::stop_token stop);
    PolledTask<Response> dispatch(const json& request);
    // Commands are registered once during construction, before dispatch is possible.
    void register_connection_commands();
    void register_data_commands();
    void register_lab_commands();
    PolledTask<void> initialized();

    ServiceIo service_io;
    // Encoded replies retain their allowance through the final bridge consumer.
    PayloadBudget snapshot_payloads{256 * 1024 * 1024};
    PayloadBudget command_payloads{128 * 1024 * 1024};
    std::uint64_t refresh_failures = 0;
    bool refresh_failed = false, settings_failed = false;
    TradingConnections trading_connections{program_directory("terminal") / "data"};
    SavedEndpoint market_endpoint{program_directory("terminal") / "data" / "market.json"};
    SavedEndpoint warehouse_endpoint{program_directory("terminal") / "data" / "warehouse.json"};
    // The open quotes watch the market pages read.
    std::shared_ptr<MarketWatch> watch;

    // The Python runtime lab runs on, installed at the first launch.
    PythonRuntime python_runtime{
        application_directory().parent_path() / "runtime",
        program_directory("terminal") / "runtime"
    };

    LabSupervisor lab{[this] {
        return python();
    }};
    std::map<std::string, Command> commands;
    IdSequence request_ids{"terminal." + unique_process_id()};
    std::uint64_t succeeded = 0, failed = 0;

    bool connecting = false, market_busy = false;
    Settings settings_view = std::make_shared<const SettingsView>();
    bool settings_busy = false;

    std::shared_ptr<const Publication> cache;
    std::stop_source lifetime;
    std::shared_future<void> initialization;
    std::future<void> refresher, settings_refresher;
};

} // namespace asterion::terminal
