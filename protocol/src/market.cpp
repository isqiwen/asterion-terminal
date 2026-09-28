#include <asterion/protocol/market.hpp>
namespace asterion::protocol {
market::v1::Snapshot encode_market(const LiveMarketSnapshot& state, const std::string& instance) {
  market::v1::Snapshot out;
  out.set_instance_id(instance);
  out.set_phase(state.phase);
  out.set_error_code(state.error_code);
  out.set_sequence(state.sequence);
  out.set_out_of_order(state.out_of_order);
  for (const auto& s : state.subscriptions) {
    auto* row = out.add_subscriptions();
    row->mutable_instrument()->set_venue(s.instrument.venue);
    row->mutable_instrument()->set_symbol(s.instrument.symbol);
    row->set_state(s.state);
    row->set_error_code(s.error_code);
    if (!s.quote)
      continue;
    const auto& q = *s.quote;
    auto* quote = row->mutable_quote();
    *quote->mutable_instrument() = row->instrument();
    if (q.last)
      quote->set_last(q.last->str());
    if (q.bid)
      quote->set_bid(q.bid->str());
    if (q.ask)
      quote->set_ask(q.ask->str());
    if (q.previous_settlement)
      quote->set_previous_settlement(q.previous_settlement->str());
    if (q.high)
      quote->set_high(q.high->str());
    if (q.low)
      quote->set_low(q.low->str());
    if (q.open_interest)
      quote->set_open_interest(q.open_interest->str());
    quote->set_bid_quantity(q.bid_quantity);
    quote->set_ask_quantity(q.ask_quantity);
    quote->set_volume(q.volume);
    quote->set_action_day(q.action_day);
    quote->set_trading_day(q.trading_day);
    quote->set_update_time(q.update_time);
    quote->set_source_ms(q.source_ms);
    quote->set_received_ms(q.received_ms);
  }
  return out;
}
market::v1::EventBatch encode_market_events(const MarketEventBatch& batch) {
  market::v1::EventBatch out;
  out.set_stream_id(batch.stream_id);
  out.set_oldest_sequence(batch.oldest_sequence);
  out.set_latest_sequence(batch.latest_sequence);
  out.set_gap(batch.gap);
  out.set_failed(batch.failed);
  for (const auto& event : batch.events) {
    auto* row = out.add_events();
    row->set_sequence(event.sequence);
    row->set_received_ms(event.received_ms);
    if (const auto* status = std::get_if<LiveMarketSnapshot>(&event.value)) {
      *row->mutable_status() = encode_market(*status, batch.stream_id);
    } else {
      const auto& observation = std::get<MarketQuoteObservation>(event.value);
      LiveMarketSnapshot single;
      single.subscriptions.push_back({observation.quote.instrument, "", 0, observation.quote});
      auto encoded = encode_market(single, batch.stream_id);
      *row->mutable_quote()->mutable_quote() = encoded.subscriptions(0).quote();
      row->mutable_quote()->set_out_of_order(observation.out_of_order);
    }
  }
  return out;
}
Json decode_market(const market::v1::Snapshot& state) {
  Json rows = Json::array();
  for (const auto& s : state.subscriptions()) {
    Json quote = nullptr;
    if (s.has_quote()) {
      const auto& q = s.quote();
      quote = {{"last", q.has_last() ? Json(q.last()) : Json(nullptr)},
               {"bid", q.has_bid() ? Json(q.bid()) : Json(nullptr)},
               {"ask", q.has_ask() ? Json(q.ask()) : Json(nullptr)},
               {"previous_settlement",
                q.has_previous_settlement() ? Json(q.previous_settlement()) : Json(nullptr)},
               {"high", q.has_high() ? Json(q.high()) : Json(nullptr)},
               {"low", q.has_low() ? Json(q.low()) : Json(nullptr)},
               {"open_interest", q.has_open_interest() ? Json(q.open_interest()) : Json(nullptr)},
               {"bid_quantity", q.bid_quantity()},
               {"ask_quantity", q.ask_quantity()},
               {"volume", q.volume()},
               {"action_day", q.action_day()},
               {"trading_day", q.trading_day()},
               {"update_time", q.update_time()},
               {"source_ms", q.source_ms()},
               {"received_ms", q.received_ms()}};
    }
    rows.push_back({{"venue", s.instrument().venue()},
                    {"symbol", s.instrument().symbol()},
                    {"state", s.state()},
                    {"error_code", s.error_code()},
                    {"quote", quote}});
  }
  return {{"instance_id", state.instance_id()},   {"phase", state.phase()},
          {"error_code", state.error_code()},     {"sequence", state.sequence()},
          {"out_of_order", state.out_of_order()}, {"subscriptions", rows}};
}
} // namespace asterion::protocol
