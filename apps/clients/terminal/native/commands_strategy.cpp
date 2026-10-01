#include "application_impl.hpp"
#include <stdexcept>

namespace asterion::terminal {
// Strategy runs: authorization handoff between the paper account and the strategy host.
void Application::Impl::register_strategy_commands() {
  core.command("strategy.run", "paper.manage", [this](const json& p) {
    fields(p, {"id", "fast", "slow", "quantity"});
    if (!paper || paper->endpoint().endpoint.empty())
      throw std::invalid_argument("automatic strategy setup currently "
                                  "requires a connected local paper account");
    const auto id = text(p, "id");
    validate_id(id);
    if (id.size() > 40 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU"
                                               "VWXYZ0123456789_-") != std::string::npos)
      throw std::invalid_argument("invalid strategy run identity");
    auto integer = [&](const char* key) {
      const auto raw = text(p, key);
      std::uint32_t value = 0;
      const auto [end, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
      if (ec != std::errc{} || end != raw.data() + raw.size() || std::to_string(value) != raw)
        throw std::invalid_argument("invalid strategy window");
      return value;
    };
    // The selected portfolio; the account accepts the grant only for its own data.
    const auto& data = selected();
    std::size_t bars = 0;
    for (const auto& item : data)
      bars += static_cast<std::size_t>(item.dataset.bars_size());
    if (bars > protocol::max_session_bars)
      throw std::invalid_argument("paper sessions use at most 20000 bars; narrow the trading days");
    strategy::v1::Config config;
    config.set_version(2);
    config.set_session_id("strategy-" + id);
    config.set_stream_id("history-" + id);
    config.set_plugin_id("asterion.strategy.cta.sma-long-flat");
    config.set_fast(integer("fast"));
    config.set_slow(integer("slow"));
    const auto quantity = Decimal::parse(text(p, "quantity"));
    config.mutable_quantity()->set_units(quantity.raw());
    const auto local = local_node();
    auto* plan = config.mutable_replay();
    plan->set_version(4);
    protocol::v1::PaperInput revisions;
    for (const auto& item : data) {
      *config.add_contracts() = item.dataset.contract();
      MovingAverage validation(protocol::instrument(item.dataset.contract()), config.fast(),
                               config.slow(), quantity);
      (void)validation;
      *plan->add_datasets() = item.dataset;
      *revisions.add_contracts()->mutable_dataset() = item.dataset;
    }
    plan->set_trading_session(paper->endpoint().session);
    plan->set_grant_id("grant." + config.session_id());
    plan->set_agent_endpoint(local.endpoint);
    static_cast<void>(protocol::decode_replay_plan(*plan));
    if (!nodes.contains("local"))
      nodes.emplace("local", std::make_shared<NodeClient>(local));
    auto next =
        std::make_unique<StrategyClient>(nodes.at("local")->local_strategy(config.session_id()));
    // Two cross-process steps: grant the account, create the strategy. There
    // is no distributed transaction; instead each step is idempotent under a
    // request identity derived from the run id (the grant id, the strategy
    // session). The trading journal acknowledges an identical repeated command
    // without re-applying it, so after a partial failure the user retries the
    // same run id and the chain resumes. A grant left without a running
    // strategy only fences manual orders until the retry succeeds or
    // strategy.revoke is called.
    paper->execute({{"request_id", plan->grant_id()},
                    {"action", "strategy_grant"},
                    {"grant_id", plan->grant_id()},
                    {"strategy_id", config.session_id()},
                    {"stream_id", config.stream_id()},
                    {"dataset_revision", protocol::dataset_revision(revisions)},
                    {"max_quantity", quantity.str()}});
    // Keep the observation handle even if delivery acknowledgement is lost.
    // The caller retains the same run id and may explicitly retry this plan.
    strategy = std::move(next);
    strategy->create(config);
    return snapshot();
  });
  core.command("strategy.attach", "node.manage", [this](const json& p) {
    fields(p, {"id", "service"});
    if (text(p, "id") == "local" && !nodes.contains("local"))
      nodes.emplace("local", std::make_shared<NodeClient>(local_node()));
    strategy = std::make_unique<StrategyClient>(
        nodes.at(text(p, "id"))
            ->service_endpoint(text(p, "service"), asterion::node::v1::STRATEGY));
    return snapshot();
  });
  core.command("strategy.revoke", "paper.manage", [this](const json& p) {
    fields(p, {"grant_id"});
    const auto grant = text(p, "grant_id");
    validate_id(grant);
    std::unique_ptr<TradingClient> observer;
    TradingClient* account = paper.get();
    if (!account || !account->snapshot().contains("strategy") ||
        account->snapshot().at("strategy").at("grant_id") != grant) {
      if (!strategy)
        throw std::invalid_argument("connect the matching paper account before revoking");
      const auto& config = strategy->config();
      const auto& plan = config.replay();
      if (plan.grant_id() != grant)
        throw std::invalid_argument("strategy authorization mismatch");
      const auto local = local_node();
      if (plan.agent_endpoint() != local.endpoint)
        throw std::invalid_argument("connect the matching paper account "
                                    "before revoking this strategy");
      if (!nodes.contains("local"))
        nodes.emplace("local", std::make_shared<NodeClient>(local));
      observer = std::make_unique<TradingClient>(
          nodes.at("local")->service_endpoint(plan.trading_session(),
                                              asterion::node::v1::PAPER_TRADING),
          TradingMode::paper);
      account = observer.get();
    }
    const auto state = account->snapshot();
    if (state.at("storage_state") != "ready" || !state.contains("strategy") ||
        state.at("strategy").at("grant_id") != grant)
      throw std::invalid_argument("strategy account authorization mismatch");
    if (state.at("strategy").at("active") == true)
      account->execute(
          {{"request_id", "revoke." + grant}, {"action", "strategy_revoke"}, {"grant_id", grant}});
    return this->snapshot();
  });
}
} // namespace asterion::terminal
