#include "application_impl.hpp"
#include <stdexcept>

namespace asterion::terminal {
namespace {

// Exchange terms of the allowed contracts from the CTP contract catalog the
// market service loaded; domestic CTP futures trade whole lots in CNY.
json policy_contracts(
    const std::shared_ptr<const json>& catalog,
    const json& requested,
    const json& retained = json::array())
{
    if (!requested.is_array() || requested.empty() || requested.size() > max_portfolio_contracts)
        throw std::invalid_argument("choose 1 to 20 contracts for the live session");
    json contracts = json::array();
    for (const auto& item : requested) {
        fields(item, {"venue", "symbol"});
        const auto known = std::ranges::find_if(retained, [&](const json& candidate) {
            return candidate.at("venue") == item.at("venue")
                && candidate.at("symbol") == item.at("symbol");
        });
        if (known != retained.end()) {
            contracts.push_back(*known);
            continue;
        }
        if (!catalog || catalog->at("phase") != "ready")
            throw std::invalid_argument(
                "load the CTP contract catalog in the market workspace first");
        const auto& rows = catalog->at("contracts");
        const auto row = std::ranges::find_if(rows, [&](const json& candidate) {
            return candidate.at("venue") == item.at("venue")
                && candidate.at("symbol") == item.at("symbol");
        });
        if (row == rows.end())
            throw std::invalid_argument("contract is not in the CTP contract catalog");
        const auto identity = HistoryIdentity::parse(row->at("contract_id").get<std::string>());
        contracts.push_back(
            {{"venue", row->at("venue")},
                {"symbol", row->at("symbol")},
                {"currency", "CNY"},
                {"price_increment", Decimal::parse(row->at("price_tick").get<std::string>()).str()},
                {"quantity_increment", "1"},
                {"multiplier", std::to_string(row->at("multiplier").get<int>())},
                {"product", identity.product},
                {"delivery_month", identity.delivery_month}});
    }
    return contracts;
}

} // namespace

std::shared_ptr<Application::Impl::LiveAccount> Application::Impl::live_account(const json& params)
{
    const auto found = live.find(text(params, "account"));
    if (found == live.end())
        throw std::invalid_argument("open this CTP account first");
    return found->second;
}

// CTP trading accounts: each account has one record and one service. Several
// may be open at once; every command names its account, and every order
// passes that account's authorization, allowlist and risk.
void Application::Impl::register_live_commands()
{
    command("live.create", [this](const json& p) -> PolledTask<Response> {
        fields_with_risk(p, {"account", "max_price_deviation", "contracts"});
        const json policy{
            {"risk", risk_parameters(p)},
            {"max_price_deviation", text(p, "max_price_deviation")},
            {"contracts",
                policy_contracts(
                    market ? market->owner_read().catalog : nullptr,
                    p.at("contracts"))}
        };
        co_await with_live_account(p, [&](TradingClient& client) -> PolledTask<void> {
            co_await PollFuture{client.create(policy)};
        });
        co_return snapshot();
    });
    command("live.open", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"account"});
        const auto id = text(p, "account");
        if (live.contains(id))
            co_return snapshot();
        auto next = co_await manage<std::shared_ptr<LiveAccount>>(
            [&]() -> PolledTask<std::shared_ptr<LiveAccount>> {
                const auto [endpoint, revision] =
                    co_await settings<std::pair<TradingEndpoint, std::string>>([&] {
                        trading_connections.prepare_identity();
                        return std::pair{
                            trading_connections.endpoint(id),
                            trading_connections.get(id).at("revision").get<std::string>()
                        };
                    });
                auto client = co_await PollFuture{TradingClient::open(service_io, endpoint)};
                co_await settings<void>([&] {
                    if (trading_connections.get(id).at("revision") != revision)
                        throw Error(
                            ErrorCode::conflict,
                            "trading connection changed; inspect again");
                });
                co_return std::make_shared<LiveAccount>(std::move(client));
            });
        live.try_emplace(id, std::move(next));
        co_return snapshot();
    });
    // The service owns its local credentials; Terminal only requests a connection.
    command("live.connect", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"account"});
        co_await with_live_account(p, [&](TradingClient& client) -> PolledTask<void> {
            (co_await PollFuture{client.connect_broker()});
        });
        co_return snapshot();
    });
    command("live.disconnect", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"account"});
        co_await with_live_account(p, [](TradingClient& client) -> PolledTask<void> {
            (co_await PollFuture{client.disconnect_broker()});
        });
        co_return snapshot();
    });
    // The account's margin and commission rates from the broker; observations
    // only, never recorded as trading commands.
    command("live.costs", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"account"});
        co_await with_live_account(p, [](TradingClient& client) -> PolledTask<void> {
            (co_await PollFuture{client.query_costs()});
        });
        co_return snapshot();
    });
    // The account is part of the request, never implied: an order cannot reach
    // another account because a different one happens to be selected.
    command("live.act", [this](const json& p) -> PolledTask<Response> {
        auto command = p;
        command.erase("account");
        const auto execute = [&](TradingClient& client) -> PolledTask<void> {
            (co_await PollFuture{client.execute(command)});
        };
        // What takes something back must not wait for, or be refused because of,
        // a command of the account in flight: a cancel, and the revoke that
        // closes its permission to send. The account's service
        // decides their order; this boundary only lets them reach it.
        if (const auto action = command.value("action", "");
            action == "cancel" || action == "live_revoke")
            co_await control_live_account(p, execute);
        else
            co_await with_live_account(p, execute);
        co_return snapshot();
    });
    command("live.policy.configure", [this](const json& p) -> PolledTask<Response> {
        fields_with_risk(
            p,
            {"account",
                "request_id",
                "account_id",
                "policy_revision",
                "max_price_deviation",
                "contracts"});
        const auto risk = risk_parameters(p);
        const auto catalog = market ? market->owner_read().catalog : nullptr;
        co_await with_live_account(p, [&](TradingClient& client) -> PolledTask<void> {
            const auto current = client.owner_view().session;
            const auto contracts =
                policy_contracts(catalog, p.at("contracts"), current->at("contracts"));
            const json command{
                {"account_id", text(p, "account_id")},
                {"policy_revision", text(p, "policy_revision")},
                {"request_id", text(p, "request_id")},
                {"action", "live_policy"},
                {"policy",
                    {{"risk", risk},
                        {"contracts", contracts},
                        {"max_price_deviation", text(p, "max_price_deviation")}}}
            };
            co_await PollFuture{client.execute(command)};
        });
        co_return snapshot();
    });
    command("live.close", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"account"});
        const auto id = text(p, "account");
        const auto found = live.find(id);
        if (found == live.end())
            co_return snapshot();
        auto account = found->second;
        if (account->busy || account->controls)
            throw Error(
                ErrorCode::conflict,
                "another operation for this CTP account is in progress; retry after it completes");
        live.erase(found);
        account->client.reset();
        co_return snapshot();
    });
}

} // namespace asterion::terminal
