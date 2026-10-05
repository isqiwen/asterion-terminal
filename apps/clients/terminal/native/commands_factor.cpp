#include "application_impl.hpp"
#include <asterion/protocol/factor.hpp>

namespace asterion::terminal {
void Application::Impl::register_factor_commands() {
  command("factor.daily.submit", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "source_dataset_id", "lookback", "horizon", "evaluation"});
    validate_id(text(p, "id"));
    auto definition = p;
    definition.erase("id");
    const auto input = protocol::encode_daily_factor_request(definition);
    if (!task_client)
      throw std::invalid_argument("task service is not connected");
    const auto client = task_client;
    const auto generation = data_task_generation;
    (co_await PollFuture{client->submit(text(p, "id"), input)});
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    co_return snapshot();
  });
  command("factor.submit", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "lookbacks", "horizon", "evaluation"});
    if (!task_client)
      throw std::invalid_argument("task service is not connected");
    if (selected().size() != 1)
      throw std::invalid_argument("factor analysis studies one contract; keep one dataset");
    const auto generation = data_task_generation;
    const auto request = protocol::encode_factor_request(
        {{"data", protocol::decode_bar_dataset_request(selected().front().request)},
         {"lookbacks", p.at("lookbacks")},
         {"horizon", p.at("horizon")},
         {"evaluation", p.at("evaluation")}});
    (co_await PollFuture{task_client->submit(text(p, "id"), request)});
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    co_return snapshot();
  });
}
} // namespace asterion::terminal
