#include "../apps/clients/terminal/native/market_snapshot.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <future>
using namespace asterion;
using namespace asterion::terminal;
namespace wire = asterion::market::v1;
namespace {
wire::Snapshot initial(int count = 3) {
  LiveMarketSnapshot state;
  state.phase = MarketPhase::connected;
  state.sequence = 10;
  for (int i = 0; i < count; ++i) {
    MarketQuote q;
    q.instrument = {"SHFE", std::string(1, static_cast<char>('a' + i / 9000)) +
                                std::to_string(1000 + i % 9000)};
    q.last = Decimal::parse("100");
    state.subscriptions.push_back({q.instrument, SubscriptionState::subscribed, 0, q});
  }
  auto out = protocol::encode_market(state, "service-instance");
  out.mutable_catalog()->set_phase("unconfigured");
  out.set_catalog_revision(1);
  return out;
}
wire::Snapshot update(const wire::Snapshot& baseline, int index, std::uint64_t sequence) {
  wire::Snapshot out;
  out.set_instance_id(baseline.instance_id());
  out.set_phase(baseline.phase());
  out.set_error_code(baseline.error_code());
  out.set_out_of_order(baseline.out_of_order());
  out.set_catalog_revision(baseline.catalog_revision());
  *out.mutable_watchlist() = baseline.watchlist();
  *out.add_subscriptions() = baseline.subscriptions(index);
  out.mutable_subscriptions(0)->mutable_quote()->set_last(std::to_string(sequence + 1000));
  out.clear_catalog();
  out.set_catalog_omitted(true);
  out.set_subscriptions_delta(true);
  out.set_base_sequence(baseline.sequence());
  out.set_sequence(sequence);
  return out;
}
Json without_revisions(Json value) {
  value.erase("subscription_set");
  value.at("catalog").erase("revision");
  for (auto& row : value.at("subscriptions"))
    row.erase("revision");
  return value;
}
} // namespace
TEST(MarketSnapshot, InterleavedCommandAndWatchPreserveExactStateAndRowRevisions) {
  MarketSnapshot view;
  auto full = initial();
  view.apply(MarketUpdate(full), view.generation());
  const auto set = view.capture().render().at("subscription_set");
  const auto unchanged = view.capture().render().at("subscriptions")[2].at("revision");
  auto first = update(full, 0, 11);
  view.apply(MarketUpdate(first), view.generation());
  *full.mutable_subscriptions(0) = first.subscriptions(0);
  full.set_sequence(11);
  auto second = update(full, 1, 12);
  *full.mutable_subscriptions(1) = second.subscriptions(0);
  full.set_sequence(12);
  // The command's full reply overtakes the older queued watch frame.
  view.apply(MarketUpdate(full), view.generation());
  view.apply(MarketUpdate(first), view.generation());
  view.apply(MarketUpdate(second), view.generation());
  EXPECT_EQ(without_revisions(view.capture().render()), protocol::decode_market(full));
  EXPECT_EQ(view.capture().render().at("subscription_set"), set);
  EXPECT_EQ(view.capture().render().at("subscriptions")[2].at("revision"), unchanged);
  auto heartbeat = second;
  heartbeat.clear_subscriptions();
  heartbeat.set_base_sequence(12);
  const auto before = view.capture().render();
  view.apply(MarketUpdate(heartbeat), view.generation());
  EXPECT_EQ(view.capture().render(), before);
  full.mutable_subscriptions()->DeleteSubrange(1, 2);
  full.set_sequence(13);
  view.apply(MarketUpdate(full), view.generation());
  EXPECT_NE(view.capture().render().at("subscription_set"), set);
  ASSERT_EQ(view.capture().render().at("subscriptions").size(), 1U);
  EXPECT_EQ(without_revisions(view.capture().render()), protocol::decode_market(full));
}
TEST(MarketSnapshot, CapturedRowsAndCatalogOutliveOwnerUpdatesAndDestruction) {
  Json before;
  std::future<Json> rendered;
  {
    struct Release {
      std::promise<void> signal;
      ~Release() { signal.set_value(); }
    } gate;
    MarketSnapshot source;
    auto full = initial();
    source.apply(MarketUpdate(full), source.generation());
    before = source.capture().render();
    rendered = std::async(std::launch::async, [projection = source.capture(),
                                               ready = gate.signal.get_future()]() mutable {
      ready.wait();
      return projection.render();
    });
    auto delta = update(full, 0, 11);
    source.apply(MarketUpdate(delta), source.generation());
    *full.mutable_subscriptions(0) = delta.subscriptions(0);
    full.set_sequence(12);
    full.set_catalog_revision(2);
    full.mutable_catalog()->set_phase("ready");
    source.apply(MarketUpdate(full), source.generation());
    EXPECT_NE(source.capture().render(), before);
    // source is destroyed before gate releases the captured reader.
  }
  EXPECT_EQ(rendered.get(), before);
}

TEST(MarketSnapshot, PreparedOldProcessCannotReplaceANewerProcess) {
  MarketSnapshot view;
  auto original = initial();
  ASSERT_TRUE(view.apply(MarketUpdate(original), view.generation()));
  const auto observed = view.generation();
  MarketUpdate delayed(original);
  auto replacement = original;
  replacement.set_instance_id("replacement");
  replacement.set_sequence(1);
  ASSERT_TRUE(view.apply(MarketUpdate(replacement), observed));
  const auto current = view.capture().render();
  EXPECT_FALSE(view.apply(std::move(delayed), observed));
  EXPECT_EQ(view.capture().render(), current);
  // A response prepared before this publication can still belong to the same
  // new process; its sequence decides whether it advances the view.
  replacement.set_sequence(2);
  EXPECT_TRUE(view.apply(MarketUpdate(replacement), observed));
  EXPECT_EQ(view.capture().render().at("sequence"), 2);
}

TEST(MarketSnapshot, RejectsMissingBaselineForeignRowsDuplicatesAndCatalogWithoutMutation) {
  auto full = initial();
  auto delta = update(full, 0, 11);
  MarketSnapshot view;
  EXPECT_THROW(view.apply(MarketUpdate(delta), view.generation()), Error);
  EXPECT_TRUE(view.capture().render().is_null());
  view.apply(MarketUpdate(full), view.generation());
  const auto before = view.capture().render();
  for (int mutation = 0; mutation < 6; ++mutation) {
    auto invalid = delta;
    switch (mutation) {
    case 0:
      invalid.set_instance_id("foreign");
      break;
    case 1:
      invalid.set_base_sequence(12);
      break;
    case 2:
      invalid.mutable_subscriptions(0)->mutable_instrument()->set_symbol("cu2701");
      break;
    case 3:
      *invalid.add_subscriptions() = delta.subscriptions(0);
      break;
    case 4:
      invalid.set_catalog_revision(2);
      break;
    case 5:
      invalid.mutable_catalog();
      break;
    }
    EXPECT_THROW(view.apply(MarketUpdate(invalid), view.generation()), Error) << mutation;
    EXPECT_EQ(view.capture().render(), before) << mutation;
  }
  full.set_base_sequence(1);
  EXPECT_THROW(view.apply(MarketUpdate(full), view.generation()), Error);
  EXPECT_EQ(view.capture().render(), before);
}
TEST(MarketSnapshot, WatchCursorRequiresContinuousFramesAndFullReconnectBaseline) {
  auto full = initial();
  auto delta = update(full, 0, 11);
  MarketWatchCursor cursor;
  EXPECT_THROW(cursor.accept(MarketUpdate(delta)), Error);
  cursor.accept(MarketUpdate(full));
  auto skipped = delta;
  skipped.set_base_sequence(9);
  EXPECT_THROW(cursor.accept(MarketUpdate(skipped)), Error);
  cursor.accept(MarketUpdate(delta));
  EXPECT_THROW(cursor.accept(MarketUpdate(full)), Error);
  full.set_instance_id("new-instance");
  full.set_sequence(1);
  cursor.accept(MarketUpdate(full));
  EXPECT_THROW(cursor.accept(MarketUpdate(delta)), Error);
  MarketWatchCursor reconnected;
  EXPECT_THROW(reconnected.accept(MarketUpdate(delta)), Error);
  EXPECT_NO_THROW(reconnected.accept(MarketUpdate(full)));
}
TEST(MarketSnapshot, TwentyThousandContractsReceiveOneThousandExactIncrementalUpdates) {
  auto full = initial(20000);
  MarketSnapshot view;
  const auto start = std::chrono::steady_clock::now();
  view.apply(MarketUpdate(full), view.generation());
  const auto set = view.capture().render().at("subscription_set");
  const auto began_updates = std::chrono::steady_clock::now();
  std::size_t bytes = 0;
  for (int i = 0; i < 1000; ++i) {
    // Build one-row frames directly; constructing a full copy is not part of
    // incremental service publication or this measured consumer workload.
    wire::Snapshot delta;
    delta.set_instance_id(full.instance_id());
    delta.set_phase(full.phase());
    delta.set_sequence(full.sequence() + 1);
    delta.set_base_sequence(full.sequence());
    delta.set_subscriptions_delta(true);
    delta.set_catalog_revision(full.catalog_revision());
    delta.set_catalog_omitted(true);
    const int index = (i * 17) % full.subscriptions_size();
    auto* row = delta.add_subscriptions();
    *row = full.subscriptions(index);
    row->mutable_quote()->set_last(std::to_string(i + 101));
    bytes += delta.ByteSizeLong();
    view.apply(MarketUpdate(delta), view.generation());
    *full.mutable_subscriptions(index) = *row;
    full.set_sequence(delta.sequence());
  }
  const auto end = std::chrono::steady_clock::now();
  EXPECT_EQ(view.capture().render().at("subscription_set"), set);
  EXPECT_EQ(without_revisions(view.capture().render()), protocol::decode_market(full));
  EXPECT_LT(bytes, full.ByteSizeLong());
  const auto ms = [](auto duration) {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
  };
  RecordProperty("initial_rows", 20000);
  RecordProperty("updates", 1000);
  RecordProperty("full_view_ms", ms(began_updates - start));
  RecordProperty("incremental_updates_ms", ms(end - began_updates));
  RecordProperty("incremental_bytes", std::to_string(bytes));
  RecordProperty("one_full_frame_bytes", std::to_string(full.ByteSizeLong()));
}
TEST(MarketSnapshot, CapturedProjectionSupportsIndependentIncrementalReaders) {
  auto full = initial(20000);
  MarketSnapshot source;
  source.apply(MarketUpdate(full), source.generation());
  const auto first = source.capture();
  auto cursor = source.cursor(), slow_cursor = cursor;
  const auto started = std::chrono::steady_clock::now();
  std::size_t changed_rows = 0;
  for (int i = 0; i < 200; ++i) {
    SCOPED_TRACE(i);
    auto delta = update(full, i, full.sequence() + 1);
    *full.mutable_subscriptions(i) = delta.subscriptions(0);
    full.set_sequence(delta.sequence());
    source.apply(MarketUpdate(delta), source.generation());
    const auto published = source.capture();
    const auto changed = published.render(cursor);
    ASSERT_TRUE(changed.at("delta"));
    ASSERT_TRUE(changed.at("catalog").at("omitted"));
    ASSERT_EQ(changed.at("subscriptions").size(), 1U);
    changed_rows += changed.at("subscriptions").size();
    cursor = source.cursor();
    EXPECT_TRUE(published.render(cursor).at("subscriptions").empty());
    if (i % 17 == 0) {
      const auto slow = published.render(slow_cursor);
      EXPECT_EQ(slow.at("subscriptions").size(), i == 0 ? 1U : 17U);
      slow_cursor = cursor;
    }
  }
  EXPECT_EQ(without_revisions(source.capture().render()), protocol::decode_market(full));
  EXPECT_EQ(first.render().at("sequence"), 10);
  RecordProperty("contracts", 20000);
  RecordProperty("background_refreshes", 200);
  RecordProperty("copied_quote_rows", std::to_string(changed_rows));
  RecordProperty("refresh_and_readers_ms",
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count()));
}
TEST(MarketSnapshot, CapturedReadersReceiveLatestRowsAndResetOnNewProcess) {
  auto full = initial();
  MarketSnapshot source;
  source.apply(MarketUpdate(full), source.generation());
  const auto old = source.cursor();
  for (int i = 0; i < 3; ++i) {
    auto delta = update(full, i % 2, full.sequence() + 1);
    *full.mutable_subscriptions(i % 2) = delta.subscriptions(0);
    full.set_sequence(delta.sequence());
    source.apply(MarketUpdate(delta), source.generation());
  }
  const auto current = source.capture();
  const auto delta = current.render(old);
  ASSERT_EQ(delta.at("subscriptions").size(), 2U);
  EXPECT_EQ(delta.at("subscriptions")[0].at("quote").at("last"), "1013");
  EXPECT_EQ(delta.at("subscriptions")[1].at("quote").at("last"), "1012");
  full.set_instance_id("replacement");
  full.set_sequence(1);
  full.mutable_subscriptions()->DeleteSubrange(1, 2);
  source.apply(MarketUpdate(full), source.generation());
  const auto replaced = source.capture().render(old);
  EXPECT_FALSE(replaced.contains("delta"));
  EXPECT_FALSE(replaced.at("catalog").contains("omitted"));
  EXPECT_EQ(replaced.at("subscriptions").size(), 1U);
  EXPECT_EQ(current.render().at("subscriptions").size(), 3U);
}
