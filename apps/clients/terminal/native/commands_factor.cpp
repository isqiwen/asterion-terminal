#include "application_impl.hpp"
#include <asterion/protocol/factor.hpp>

namespace asterion::terminal {
void Application::Impl::register_factor_commands() {
  command("factor.submit", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "series", "lookbacks", "horizon", "evaluation"});
    validate_id(text(p, "id"));
    // A factor reads one series: the bars this window has selected, or a
    // daily version published in the archive.
    const auto& series = p.at("series");
    json source;
    if (series.is_object() && series.value("kind", "") == "bars") {
      fields(series, {"kind"});
      if (selected().size() != 1)
        throw std::invalid_argument("factor analysis studies one contract; keep one dataset");
      source = {{"bars", protocol::decode_bar_dataset_request(selected().front().request)}};
    } else if (series.is_object() && series.value("kind", "") == "daily") {
      fields(series, {"kind", "dataset_id"});
      source = {{"daily_dataset_id", series.at("dataset_id")}};
    } else
      throw std::invalid_argument("invalid factor series");
    const auto request = protocol::encode_factor_request({{"series", json::array({source})},
                                                          {"lookbacks", p.at("lookbacks")},
                                                          {"horizon", p.at("horizon")},
                                                          {"evaluation", p.at("evaluation")}});
    if (!task_client)
      throw std::invalid_argument("task service is not connected");
    const auto client = task_client;
    const auto generation = data_task_generation;
    (co_await PollFuture{client->submit(text(p, "id"), request)});
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    co_return snapshot();
  });
}
} // namespace asterion::terminal
