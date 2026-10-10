#include "application_impl.hpp"
#include <asterion/protocol/factor.hpp>

namespace asterion::terminal {
void Application::Impl::register_backtest_commands() {
  command("backtest.submit", [this](const json& p) -> PolledTask<Response> {
    fields_with_risk(p, {"id", "fast", "slow", "quantity", "sides", "deposit", "contracts"});
    if (!task_client)
      throw std::invalid_argument("task service is not connected");
    const auto generation = data_task_generation;
    const auto costs = selection_costs(p.at("contracts"));
    json contracts = json::array(), series = json::array();
    for (std::size_t i = 0; i < selected().size(); ++i)
      if (!series_of(selected()[i].dataset.contract()))
        contracts.push_back({{"data", protocol::decode_bar_dataset_request(selected()[i].request)},
                             {"cost_schedule", costs[i]}});
    // A series sends every month it was resolved from, so the service works
    // out the same schedule. A month that never trades takes the costs of
    // one that does; they are not used.
    for (const auto& item : dataset_series) {
      json members = json::array();
      std::optional<json> any;
      std::vector<std::optional<json>> known;
      for (const auto& month : item.months) {
        std::optional<json> cost;
        for (std::size_t i = 0; i < selected().size(); ++i)
          if (selected()[i].dataset.contract().venue() == month.contract().venue() &&
              selected()[i].dataset.contract().symbol() == month.contract().symbol())
            cost = costs[i];
        if (cost && !any)
          any = cost;
        known.push_back(std::move(cost));
      }
      for (std::size_t m = 0; m < item.months.size(); ++m) {
        members.push_back(contracts.size());
        contracts.push_back({{"data", protocol::decode_bar_dataset_request(item.months[m])},
                             {"cost_schedule", known[m] ? *known[m] : any.value()}});
      }
      series.push_back(std::move(members));
    }
    auto request = protocol::encode_backtest_request({{"contracts", std::move(contracts)},
                                                      {"deposit", text(p, "deposit")},
                                                      {"risk", risk_parameters(p)},
                                                      {"sma",
                                                       {{"fast", p.at("fast")},
                                                        {"slow", p.at("slow")},
                                                        {"quantity", text(p, "quantity")},
                                                        {"sides", p.at("sides")}}},
                                                      {"series", std::move(series)}});
    (co_await PollFuture{task_client->submit(text(p, "id"), request)});
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    co_return snapshot();
  });
}
} // namespace asterion::terminal
