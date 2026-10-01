#pragma once
#include <asterion/foundation/serialization.hpp>
#include <asterion/v1/market.pb.h>
#include <deque>
#include <algorithm>
#include <quote_volume.hpp>
#include <limits>
#include <stdexcept>

namespace asterion::terminal {
// Bounded presentation history from the event stream, never sampled snapshots.
class MarketHistory {
public:
  std::string stream;
  std::uint64_t cursor = 0;
  void interrupt() {
    stream.clear();
    cursor = 0;
    available_ = false;
    interrupted_ = true;
    clear();
  }
  void append(const market::v1::EventBatch& batch) {
    if (batch.stream_id().empty() || batch.latest_sequence() < cursor)
      throw std::invalid_argument("invalid market history batch");
    if (!stream.empty() && stream != batch.stream_id())
      throw std::invalid_argument("market history stream changed");
    auto previous = cursor;
    bool first = true;
    for (const auto& event : batch.events()) {
      if (event.sequence() <= previous || event.sequence() > batch.latest_sequence() ||
          (!(batch.gap() && first) && event.sequence() != previous + 1))
        throw std::invalid_argument("invalid market history sequence");
      previous = event.sequence();
      first = false;
    }
    stream = batch.stream_id();
    if (batch.gap() || batch.failed()) {
      clear();
      interrupted_ = true;
    }
    available_ = !batch.failed();
    if (batch.failed()) {
      cursor = batch.latest_sequence();
      return;
    }
    for (const auto& event : batch.events()) {
      cursor = event.sequence();
      if (event.has_status()) {
        if (event.status().phase() != "connected") {
          if (!points_.empty())
            interrupted_ = true;
          clear();
        }
        continue;
      }
      if (!event.has_quote() || event.quote().out_of_order())
        continue;
      const auto& quote = event.quote().quote();
      if (!quote.has_last() || quote.source_ms() <= 0 ||
          quote.source_ms() > std::numeric_limits<std::int64_t>::max() / 1000000)
        continue;
      const auto same_instrument = [&](const Observation& point) {
        return point.value.at("venue") == quote.instrument().venue() &&
               point.value.at("symbol") == quote.instrument().symbol();
      };
      const auto previous_quote = std::find_if(points_.rbegin(), points_.rend(), same_instrument);
      std::optional<std::int64_t> volume;
      if (previous_quote != points_.rend()) {
        volume = chart_indicators::quote_volume(previous_quote->day, previous_quote->volume,
                                                quote.trading_day(), quote.volume());
        if (previous_quote->day != quote.trading_day() || quote.volume() < previous_quote->volume ||
            quote.volume() < 0) {
          // A new session/counter must never be joined to the old price curve.
          std::erase_if(points_, same_instrument);
          ++generation_;
        }
      }
      Json point = {{"venue", quote.instrument().venue()},
                    {"symbol", quote.instrument().symbol()},
                    {"timestamp_ns", std::to_string(quote.source_ms() * 1000000)},
                    {"price", quote.last()}};
      if (volume)
        point["volume"] = std::to_string(*volume);
      points_.push_back({std::move(point), quote.trading_day(), quote.volume()});
      if (points_.size() > 512)
        points_.pop_front();
    }
  }
  Json snapshot() const {
    Json points = Json::array();
    for (const auto& point : points_)
      points.push_back(point.value);
    return {{"stream_id", stream},
            {"generation", std::to_string(generation_)},
            {"available", available_},
            {"interrupted", interrupted_},
            {"points", std::move(points)}};
  }

private:
  void clear() {
    points_.clear();
    ++generation_;
  }
  struct Observation {
    Json value;
    std::string day;
    std::int64_t volume;
  };
  std::deque<Observation> points_;
  bool available_ = false;
  bool interrupted_ = false;
  std::uint64_t generation_ = 0;
};
} // namespace asterion::terminal
