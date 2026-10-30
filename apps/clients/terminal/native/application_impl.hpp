#pragma once
// Internal to the Terminal application: shared state of Application and the
// per-domain command registrations (commands_*.cpp). Not a public API.
#include "terminal_application.hpp"
#include <asterion/domain/history_identity.hpp>
#include "market_client.hpp"
#include "local_services.hpp"
#include "node_paths.hpp"
#include "ctp_connections.hpp"
#include "trading_connections.hpp"
#include "data_credentials.hpp"
#include "plugin_catalog.hpp"
#include "research_client.hpp"
#include "data_client.hpp"
#include "trading_client.hpp"
#include "service_io.hpp"
#include <asterion/domain/futures.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/log.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/foundation/time.hpp>
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
json risk_parameters(const json& p);
// fields() for a request that also carries the three risk limits.
void fields_with_risk(const json& object, std::initializer_list<std::string_view> names);
std::string text(const json& object, const char* name, bool allow_empty = false);
unsigned short port_number(const json& p, const char* name);
data::proto::MinutePageQuery minute_page_query(const json& params);

// Bars selected for backtests and factors: completed downloads resolved by
// the data service, never local files.
struct DatasetSelection
{
    data::proto::BarDatasetRequest request;
    data::proto::BarDataset dataset;
    json summary;
};

// Month contracts of one product selected together as its dominant series.
// The selections hold the months that trade. A submission sends again what the
// series was resolved from: every month it was given, or the product whose
// months the daily table names.
struct DatasetSeries
{
    std::vector<data::proto::BarDatasetRequest> months;
    std::optional<data::proto::ProductBarsRequest> product;
    json summary;
};

struct Application::Impl
{
    struct Publication;

    // A command fixes its publication on the owner. Queues retain references and
    // query-specific values; full JSON exists only while a response worker uses it.
    struct Response
    {
        std::shared_ptr<const Publication> publication;
        std::optional<MarketCursor> cursor;
        json values;

        Response(json value)
            : values(std::move(value))
        {}

        Response(std::shared_ptr<const Publication> state, std::optional<MarketCursor> cursor)
            : publication(std::move(state)),
              cursor(cursor),
              values(json::object())
        {}

        json render() &&;
    };

    ServiceIo service_io;
    // Encoded replies retain their allowance through the final bridge consumer.
    PayloadBudget snapshot_payloads{256 * 1024 * 1024};
    PayloadBudget command_payloads{128 * 1024 * 1024};
    std::uint64_t refresh_failures = 0;
    bool refresh_failed = false, settings_failed = false;
    void record_refresh_failure(ErrorCode code) noexcept;
    std::shared_ptr<ResearchClient> research_client;
    std::shared_ptr<DataClient> data_client;
    std::uint64_t data_task_generation = 0;

    bool task_owner_is_data(const json& p) const
    {
        const auto& owner = p.at("owner");
        fields(owner, {"kind", "node_id", "service", "connection_id"});
        const auto kind = text(owner, "kind");
        json current;
        if (kind == "data" && data_client)
            current = data_client->owner_status();
        else if (kind == "research" && research_client)
            current = research_client->owner_view().connection;
        else
            throw Error(
                ErrorCode::conflict,
                "task owner selection changed; inspect the original service");
        for (const auto* field : {"node_id", "service", "connection_id"})
            if (current.at(field) != text(owner, field))
                throw Error(
                    ErrorCode::conflict,
                    "task owner selection changed; inspect the original service");
        return kind == "data";
    }

    data::proto::DataOwner research_data_owner() const
    {
        if (!data_client || !research_client)
            throw Error(ErrorCode::unavailable, "select data and research services");
        const auto data = data_client->endpoint();
        const auto research = research_client->endpoint();
        if (data.owner_node_id.empty() || research.owner_node_id.empty()
            || data.owner_node_id != research.owner_node_id)
            throw Error(ErrorCode::conflict, "cross-node data access requires a remote resolver");
        data::proto::DataOwner owner;
        owner.set_node_id(data.owner_node_id);
        owner.set_service_id(data.session);
        return owner;
    }

    void adopt_data(std::shared_ptr<DataClient> next);
    void adopt_research(std::shared_ptr<ResearchClient> next);
    std::shared_ptr<const json> native_plugins;
    // Remembered credentials live in the keychain; the helper is found next to
    // the Terminal programs or through ASTERION_KEYCHAIN_EXECUTABLE.
    std::shared_ptr<CredentialStore> credential_store = keychain_store(keychain_helper());
    DataCredentials data_credentials{
        program_directory("terminal") / "data" / "data-providers",
        credential_store
    };
    json credential_verification = nullptr;
    CtpConnections ctp_connections{
        program_directory("terminal") / "data" / "ctp-connections",
        credential_store
    };
    TradingConnections trading_connections{program_directory("terminal") / "data"};
    // The one CTP account that supplies market data.
    PolledTask<CtpConnection> market_ctp();
    // Where a CTP account keeps its trading record; one record per account.
    // Stored accounts, each with whether it has a trading record.
    json ctp_accounts() const;
    // The credential a download of this source uses: the one typed for this
    // request, otherwise the one saved for the source's provider.
    PolledTask<std::string> source_credential(const std::string& source, std::string typed);
    // The credential typed for this request, else the one saved for the provider.
    PolledTask<std::string> provider_credential(const std::string& provider, std::string typed);
    std::shared_ptr<const json> task_result;
    // One dataset per portfolio contract, in selection order.
    std::vector<DatasetSelection> selections;
    std::vector<DatasetSeries> dataset_series;
    // The series a selected contract is a month of, if any.
    std::optional<std::size_t> series_of(const protocol::proto::Contract& contract) const;
    // One contract's dataset joins the selection, or replaces the contract's.
    void adopt_selection(DatasetSelection next);
    // A resolved series replaces whatever was selected of its product: `traded`
    // are the requests of the months that trade, in the preview's order.
    void adopt_series(
        DatasetSeries series,
        const data::proto::DominantSeriesPreview& preview,
        const std::vector<data::proto::BarDatasetRequest>& traded);
    std::uint64_t dataset_selection_generation = 0;
    // The selected datasets or an actionable error; never an empty stand-in.
    const std::vector<DatasetSelection>& selected() const;
    // What every selected contract trades at, in selection order: its cost
    // schedule and slippage. A request's "contracts" has one entry for each
    // contract outside a series ({venue, symbol, cost_schedule, slippage_ticks})
    // and one for each series ({venue, product, ...}), which all its months take.
    std::vector<json> selection_costs(const json& members) const;
    std::shared_ptr<MarketClient> market;

    // Open CTP trading accounts by account id; each has its own service.
    struct LiveAccount
    {
        std::shared_ptr<TradingClient> client;
        // Commands for the same account conflict; observation uses the I/O owner.
        bool busy = false;
        // Controls do not conflict with that command: a cancel, a strategy stop
        // and a revoke reach the account's service, which orders them.
        unsigned controls = 0;

        explicit LiveAccount(std::shared_ptr<TradingClient> connection)
            : client(std::move(connection))
        {}
    };

    std::map<std::string, std::shared_ptr<LiveAccount>> live;
    std::shared_ptr<LiveAccount> live_account(const json& params);

    struct ResetFlag
    {
        bool& flag;

        ~ResetFlag()
        {
            flag = false;
        }
    };

    template <class F>
    PolledTask<void> with_live_account(const json& params, F io)
    {
        const auto account = live_account(params);
        if (account->busy)
            throw Error(
                ErrorCode::conflict,
                "another operation for this CTP account is in progress; retry after it completes");
        account->busy = true;
        ResetFlag reset{account->busy};
        co_await io(*account->client);
    }

    template <class F>
    PolledTask<void> control_live_account(const json& params, F io)
    {
        const auto account = live_account(params);
        ++account->controls;

        struct Done
        {
            unsigned& controls;

            ~Done()
            {
                --controls;
            }
        } done{account->controls};

        co_await io(*account->client);
    }

    // The local business services; the Terminal starts and supervises them.
    LocalServices services;
    // UI commands belong to this Native boundary; registration is constructor-only.
    using Command = std::function<PolledTask<Response>(const json&)>;
    std::map<std::string, Command> commands;
    void command(std::string name, Command handler);
    PolledTask<Response> invoke(const std::string& method, const json& params);
    IdSequence request_ids{"terminal." + unique_process_id()};
    std::uint64_t succeeded = 0, failed = 0;

    // Coroutine factories retain their captures until their child operation finishes.
    template <class T, class F>
    static PolledTask<T> run(F work)
    {
        co_return co_await work();
    }

    bool management_busy = false, market_busy = false;

    template <class T, class F>
    PolledTask<T> manage(F work)
    {
        if (management_busy)
            throw Error(
                ErrorCode::conflict,
                "another node operation is in progress; retry after it completes");
        management_busy = true;
        ResetFlag reset{management_busy};
        co_return co_await work();
    }

    // Settings files and the keychain are used by one admitted management job.
    // Only the returned display projection is applied to the I/O owner's state.
    struct SettingsView
    {
        json credentials = json::array(), accounts = json::array(), market = nullptr;
        json trading = json::array();
        std::string trading_fingerprint;
        bool operator==(const SettingsView&) const = default;
    };

    using Settings = std::shared_ptr<const SettingsView>;
    Settings settings_view = std::make_shared<const SettingsView>();
    bool settings_busy = false;

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
    // data.history.usage: references across services and nodes.
    PolledTask<Response> history_usage(const json& params);

    // The owner fixes these references and their revision before any rendering.
    struct Publication
    {
        json metadata;
        std::vector<NodeSnapshot> nodes;
        Settings settings;
        std::map<std::string, TradingClient::Read> live;
        std::optional<ResearchClient::Read> tasks;
        MarketProjection market;
        std::shared_ptr<const json> task_result, plugins;
        std::uint64_t revision = 0;
        std::int64_t refreshed_at_ms = 0;
        bool same_state(const Publication&) const;
        json render(std::optional<MarketCursor>) const;
    };

    std::shared_ptr<const Publication> cache;
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
    void register_live_commands();
    void register_node_commands();
    void register_backup_commands();
    void register_data_commands();
    void register_task_commands();
    void register_backtest_commands();
    void register_factor_commands();
    void register_data_task_connection_commands();
    void register_connection_commands();
    void register_market_commands();
    void register_lab_commands();
    std::stop_source lifetime;
    std::shared_future<void> initialization;
    PolledTask<void> initialized();
    std::future<void> refresher, settings_refresher;
};

} // namespace asterion::terminal
