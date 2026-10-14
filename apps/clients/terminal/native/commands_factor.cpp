#include "application_impl.hpp"
#include <asterion/protocol/factor.hpp>

namespace asterion::terminal {
void Application::Impl::register_factor_commands() {
  command("factor.submit", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "series", "factor", "lookbacks", "horizon", "evaluation"});
    validate_id(text(p, "id"));
    // A factor reads the bars this window has selected, or instruments of the
    // market-wide daily table over the ranges given for them.
    // Each selected contract is a series, and so is each dominant series, which
    // sends every month it was resolved from, or the product whose months Data
    // finds in the daily table.
    const auto& series = p.at("series");
    auto sources = json::array();
    if (series.is_object() && series.value("kind", "") == "bars") {
      fields(series, {"kind"});
      for (const auto& dataset : selected())
        if (!series_of(dataset.dataset.contract()))
          sources.push_back({{"bars", protocol::decode_bar_dataset_request(dataset.request)}});
      for (const auto& item : dataset_series) {
        if (item.product) {
          sources.push_back(
              {{"dominant", {{"product", protocol::decode_product_bars_request(*item.product)}}}});
          continue;
        }
        auto months = json::array();
        for (const auto& month : item.months)
          months.push_back(protocol::decode_bar_dataset_request(month));
        sources.push_back({{"dominant", std::move(months)}});
      }
    } else if (series.is_object() && series.value("kind", "") == "tables") {
      fields(series, {"kind", "snapshot", "daily_source", "calendar_source", "end_day", "ranges"});
      // Each range is asked of Data by itself: instruments of an exchange that
      // began inside the days asked for are read from the day it began.
      const auto& ranges = series.at("ranges");
      if (!ranges.is_array() || ranges.empty())
        throw std::invalid_argument("invalid factor series");
      for (const auto& range : ranges) {
        fields(range, {"begin_day", "instruments"});
        sources.push_back({{"tables",
                            {{"snapshot", series.at("snapshot")},
                             {"daily_source", series.at("daily_source")},
                             {"calendar_source", series.at("calendar_source")},
                             {"begin_day", range.at("begin_day")},
                             {"end_day", series.at("end_day")},
                             {"instruments", range.at("instruments")}}}});
      }
    } else
      throw std::invalid_argument("invalid factor series");
    const auto request = protocol::encode_factor_request({{"series", std::move(sources)},
                                                          {"factor", p.at("factor")},
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
