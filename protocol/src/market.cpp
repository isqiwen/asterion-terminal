#include <asterion/protocol/market.hpp>
namespace asterion::protocol {
market::v1::Snapshot encode_market(const LiveMarketSnapshot& state, const std::string& instance) {
  market::v1::Snapshot out;
  out.set_subscriptions_delta(state.subscriptions_delta);
  out.set_instance_id(instance);
  out.set_phase(std::string(market_phase_name(state.phase)));
  out.set_error_code(state.error_code);
  out.set_sequence(state.sequence);
  out.set_out_of_order(state.out_of_order);
  for (const auto& s : state.subscriptions) {
    auto* row = out.add_subscriptions();
    row->mutable_instrument()->set_venue(s.instrument.venue);
    row->mutable_instrument()->set_symbol(s.instrument.symbol);
    row->set_state(std::string(subscription_state_name(s.state)));
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
    if (q.previous_close)
      quote->set_previous_close(q.previous_close->str());
    if (q.open_interest_change)
      quote->set_open_interest_change(q.open_interest_change->str());
    if (q.average_price)
      quote->set_average_price(q.average_price->str());
    if (q.open)
      quote->set_open(q.open->str());
    if (q.upper_limit)
      quote->set_upper_limit(q.upper_limit->str());
    if (q.lower_limit)
      quote->set_lower_limit(q.lower_limit->str());
    if (q.high)
      quote->set_high(q.high->str());
    if (q.low)
      quote->set_low(q.low->str());
    if (q.open_interest)
      quote->set_open_interest(q.open_interest->str());
    quote->set_bid_quantity(q.bid_quantity);
    quote->set_ask_quantity(q.ask_quantity);
    quote->set_volume(q.volume);
    const auto depth = [](const auto& levels, auto* output) {
      for (const auto& level : levels) {
        if (level.quantity && (!level.price || *level.quantity < 0))
          throw std::invalid_argument("invalid market depth level");
        auto* row = output->Add();
        if (level.price)
          row->set_price(level.price->str());
        if (level.quantity)
          row->set_quantity(*level.quantity);
      }
    };
    depth(q.bid_levels, quote->mutable_bid_levels());
    depth(q.ask_levels, quote->mutable_ask_levels());
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
    } else if (const auto* subscription = std::get_if<MarketSubscription>(&event.value)) {
      if (subscription->quote)
        throw std::invalid_argument("subscription acknowledgement cannot carry a quote");
      auto* state = row->mutable_subscription();
      state->mutable_instrument()->set_venue(subscription->instrument.venue);
      state->mutable_instrument()->set_symbol(subscription->instrument.symbol);
      state->set_state(std::string(subscription_state_name(subscription->state)));
      state->set_error_code(subscription->error_code);
    } else {
      const auto& observation = std::get<MarketQuoteObservation>(event.value);
      LiveMarketSnapshot single;
      single.subscriptions.push_back(
          {observation.quote.instrument, SubscriptionState::subscribed, 0, observation.quote});
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
      const auto depth = [](const auto& levels) {
        if (levels.size() != 4)
          throw std::invalid_argument("invalid market depth level count");
        Json rows = Json::array();
        for (const auto& level : levels) {
          if (level.has_quantity() && (!level.has_price() || level.quantity() < 0))
            throw std::invalid_argument("invalid market depth level");
          if (level.has_price())
            (void)Decimal::parse(level.price());
          rows.push_back(
              {{"price", level.has_price() ? Json(level.price()) : Json(nullptr)},
               {"quantity", level.has_quantity() ? Json(level.quantity()) : Json(nullptr)}});
        }
        return rows;
      };
      const auto optional_price = [](bool present, const std::string& value) -> Json {
        if (!present)
          return nullptr;
        (void)Decimal::parse(value);
        return value;
      };
      quote = {{"previous_close", optional_price(q.has_previous_close(), q.previous_close())},
               {"open_interest_change",
                optional_price(q.has_open_interest_change(), q.open_interest_change())},
               {"open", optional_price(q.has_open(), q.open())},
               {"average_price", optional_price(q.has_average_price(), q.average_price())},
               {"upper_limit", optional_price(q.has_upper_limit(), q.upper_limit())},
               {"lower_limit", optional_price(q.has_lower_limit(), q.lower_limit())},
               {"bid_levels", depth(q.bid_levels())},
               {"ask_levels", depth(q.ask_levels())},
               {"last", q.has_last() ? Json(q.last()) : Json(nullptr)},
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
                    {"change_1m_percent", s.has_change_1m_percent()
                                              ? Json(Decimal::parse(s.change_1m_percent()).str())
                                              : Json(nullptr)},
                    {"quote", std::move(quote)}});
  }
  Json catalog_rows = Json::array(), watchlist = Json::array();
  for (const auto& row : state.catalog().contracts()) {
    (void)Decimal::parse(row.price_tick());
    catalog_rows.push_back({{"venue", row.instrument().venue()},
                            {"symbol", row.instrument().symbol()},
                            {"name", row.name()},
                            {"product", row.product()},
                            {"expiry", row.expiry()},
                            {"contract_id", row.contract_id()},
                            {"multiplier", row.multiplier()},
                            {"price_tick", row.price_tick()}});
  }
  for (const auto& row : state.watchlist())
    watchlist.push_back({{"venue", row.venue()}, {"symbol", row.symbol()}});
  return {{"catalog",
           {{"phase", state.catalog().phase().empty() ? "unconfigured" : state.catalog().phase()},
            {"error_code", state.catalog().error_code()},
            {"diagnostic", state.catalog().diagnostic()},
            {"trading_day", state.catalog().trading_day()},
            {"contracts", std::move(catalog_rows)}}},
          {"watchlist", std::move(watchlist)},
          {"instance_id", state.instance_id()},
          {"phase", state.phase()},
          {"error_code", state.error_code()},
          {"sequence", state.sequence()},
          {"out_of_order", state.out_of_order()},
          {"subscriptions", std::move(rows)}};
}
market::v1::MinuteSeries encode_minutes(const IntradaySeries& series) {
  market::v1::MinuteSeries out;
  out.mutable_instrument()->set_venue(series.instrument.venue);
  out.mutable_instrument()->set_symbol(series.instrument.symbol);
  out.set_trading_day(series.trading_day);
  out.set_first_observation_ms(series.first_observation_ms);
  out.set_interrupted(series.interrupted);
  if (series.previous_settlement)
    out.set_previous_settlement(series.previous_settlement->str());
  for (const auto& bar : series.bars) {
    auto* row = out.add_bars();
    row->set_start_ms(bar.start_ms);
    row->set_open(bar.open.str());
    row->set_high(bar.high.str());
    row->set_low(bar.low.str());
    row->set_close(bar.close.str());
    row->set_volume(bar.volume);
    if (bar.average_price)
      row->set_average_price(bar.average_price->str());
    if (bar.open_interest)
      row->set_open_interest(bar.open_interest->str());
  }
  return out;
}
Json decode_minutes(const market::v1::MinuteSeries& series) {
  Json bars = Json::array();
  std::int64_t previous = 0;
  for (const auto& bar : series.bars()) {
    const auto open = Decimal::parse(bar.open()), high = Decimal::parse(bar.high()),
               low = Decimal::parse(bar.low()), close = Decimal::parse(bar.close());
    if (bar.start_ms() <= previous || bar.start_ms() % 60000 != 0 || bar.volume() < 0 ||
        low > high || open < low || open > high || close < low || close > high)
      throw std::invalid_argument("invalid market minute bar");
    previous = bar.start_ms();
    bars.push_back(
        {{"start_ms", bar.start_ms()},
         {"open", bar.open()},
         {"high", bar.high()},
         {"low", bar.low()},
         {"close", bar.close()},
         {"volume", bar.volume()},
         {"average_price", bar.has_average_price() ? Json(Decimal::parse(bar.average_price()).str())
                                                   : Json(nullptr)},
         {"open_interest", bar.has_open_interest() ? Json(Decimal::parse(bar.open_interest()).str())
                                                   : Json(nullptr)}});
  }
  return {{"venue", series.instrument().venue()},
          {"symbol", series.instrument().symbol()},
          {"trading_day", series.trading_day()},
          {"first_observation_ms", series.first_observation_ms()},
          {"interrupted", series.interrupted()},
          {"previous_settlement", series.has_previous_settlement()
                                      ? Json(Decimal::parse(series.previous_settlement()).str())
                                      : Json(nullptr)},
          {"bars", std::move(bars)}};
}
} // namespace asterion::protocol
