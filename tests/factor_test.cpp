#include "factor_engine.hpp"
#include "bar_fixture.hpp"
#include "task_store.hpp"
#include "task_store_support.hpp"
#include "daily_momentum.hpp"
#include "momentum.hpp"
#include <asterion/kernel/process/child.hpp>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
using namespace asterion;
namespace {
factor::v1::FactorInput search_input();
Decimal d(const char* value) {
  return Decimal::parse(value);
}
Instrument spec() {
  return {{"SHFE", "rb2610"}, "CNY", d("1"), d("1"), d("10")};
}
void revision(factor::v1::FactorInput&);
factor::v1::FactorInput input() {
  factor::v1::FactorInput result;
  result.set_version(5);
  result.set_full_sample(true);
  result.add_lookbacks(2);
  result.set_horizon(1);
  for (int i = 0; i < 40; ++i) {
    auto* t = result.mutable_dataset()->add_bars();
    t->set_timestamp_ns(1790298000000000000LL + static_cast<std::int64_t>(i) * 1000000000);
    t->mutable_close()->set_units(Decimal::parse(std::to_string(100 + i + i % 3)).raw());
    t->mutable_volume()->set_units(d("1").raw());
  }
  revision(result);
  return result;
}
void revision(factor::v1::FactorInput& value) {
  std::vector<MarketBar> bars;
  for (const auto& row : value.dataset().bars()) {
    const auto price = Decimal::from_raw(row.close().units());
    bars.push_back({"2026-09-25", row.timestamp_ns(), price, price, price, price,
                    Decimal::from_raw(row.volume().units())});
  }
  *value.mutable_dataset() = test::dataset(bars);
  value.set_dataset_revision(value.dataset().revision());
}
} // namespace
TEST(Factor, StreamingWarmupLifecycleAndRejectedEventPreservesHistory) {
  MomentumFactor factor(spec(), 2);
  auto first = test::flat("2026-09-25", 1, "100", "1");
  EXPECT_THROW(factor.on_bar(first), std::logic_error);
  factor.start();
  EXPECT_FALSE(factor.on_bar(first));
  EXPECT_FALSE(factor.on_bar(test::flat("2026-09-25", 2, "105", "1")));
  EXPECT_THROW(factor.on_bar(test::flat("2026-09-25", 1, "110", "1")), std::invalid_argument);
  EXPECT_THROW(factor.on_bar(test::flat("2026-09-25", 3, "0", "1")), std::invalid_argument);
  EXPECT_NEAR(*factor.on_bar(test::flat("2026-09-25", 3, "110", "1")), .1, 1e-15);
  factor.stop();
  factor.start();
  EXPECT_FALSE(factor.on_bar(first));
  EXPECT_THROW(MomentumFactor(spec(), 0), std::invalid_argument);
}
TEST(Factor, PearsonTiedRanksUndefinedVarianceAndSmallPriceChanges) {
  const std::vector<double> x{1, 1, 2, 3}, y{4, 3, 2, 1};
  EXPECT_NEAR(*rank_correlation(x, y), -std::sqrt(.9), 1e-14);
  EXPECT_NEAR(*correlation(x, x), 1, 1e-14);
  EXPECT_FALSE(correlation(std::vector<double>{2, 2, 2}, std::vector<double>{1, 2, 3}));
  EXPECT_FALSE(rank_correlation(std::vector<double>{1, 2, 3}, std::vector<double>{2, 2, 2}));
  EXPECT_THROW(correlation(std::vector<double>{1, 2}, std::vector<double>{1}),
               std::invalid_argument);
  EXPECT_THROW(rank_correlation(std::vector<double>{1, std::numeric_limits<double>::quiet_NaN()},
                                std::vector<double>{1, 2}),
               std::invalid_argument);
  const std::vector<double> huge{-1e308, 0, 1e308};
  EXPECT_NEAR(*correlation(huge, huge), 1, 1e-14);
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  EXPECT_GT(price_return(Decimal::from_raw(maximum - 1), Decimal::from_raw(maximum)), 0);
  EXPECT_LT(price_return(Decimal::from_raw(maximum), Decimal::from_raw(maximum - 1)), 0);
}
TEST(Factor, ExactDatasetDigestExcludesWindowsAndRejectsTampering) {
  auto value = input();
  const auto hash = value.dataset_revision();
  value.set_lookbacks(0, 3);
  EXPECT_EQ(protocol::bar_dataset_revision(value.dataset()), hash);
  auto metadata = protocol::decode_factor(value);
  metadata.at("dataset").erase("bars");
  metadata.at("dataset").erase("days");
  EXPECT_EQ(protocol::decode_factor(value, protocol::DatasetView::metadata), metadata);
  value.mutable_dataset()->mutable_bars(0)->mutable_volume()->set_units(d("2").raw());
  EXPECT_THROW(protocol::decode_factor(value, protocol::DatasetView::metadata),
               std::invalid_argument);
  EXPECT_THROW(protocol::validate_factor_input(value), std::invalid_argument);
  value = input();
  value.set_version(1);
  EXPECT_THROW(protocol::validate_factor_input(value), std::invalid_argument);
  value = input();
  value.mutable_dataset()->mutable_bars(0)->clear_close();
  EXPECT_THROW(protocol::validate_factor_input(value), std::invalid_argument);
}
TEST(Factor, AlignmentTailExclusionAndNoFutureInputs) {
  auto value = input();
  const auto result = factor::run(value);
  ASSERT_EQ(result.samples_size(), 37);
  EXPECT_EQ(result.samples(0).event_index(), 2U);
  EXPECT_EQ(result.samples(0).timestamp_ns(), value.dataset().bars(2).timestamp_ns());
  EXPECT_EQ(result.samples(0).label_timestamp_ns(), value.dataset().bars(3).timestamp_ns());
  EXPECT_NEAR(result.samples(0).value(), .04, 1e-15);
  EXPECT_NEAR(result.samples(0).forward_return(), -1.0 / 104, 1e-15);
  EXPECT_EQ(result.samples(result.samples_size() - 1).event_index(), 38U);
  value.mutable_dataset()->mutable_bars(30)->mutable_close()->set_units(d("500").raw());
  revision(value);
  const auto modified = factor::run(value);
  for (int i = 0; i < result.samples_size(); ++i) {
    const auto index = result.samples(i).event_index();
    if (index < 30)
      EXPECT_EQ(result.samples(i).value(), modified.samples(i).value());
    if (index + value.horizon() < 30)
      EXPECT_EQ(result.samples(i).forward_return(), modified.samples(i).forward_return());
  }
  EXPECT_EQ(result.SerializeAsString(), factor::run(input()).SerializeAsString());
}
TEST(Factor, InvalidOrderingInsufficientSamplesAndCancellation) {
  auto value = input();
  value.set_lookbacks(0, 10);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  value = input();
  value.mutable_dataset()->mutable_bars(4)->set_timestamp_ns(
      value.dataset().bars(0).timestamp_ns());
  EXPECT_THROW(revision(value), std::invalid_argument);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  value = input();
  value.mutable_dataset()->mutable_bars(4)->mutable_close()->set_units(0);
  revision(value);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  std::stop_source stop;
  std::size_t done = 0;
  EXPECT_THROW(factor::run(input(), stop.get_token(),
                           [&](auto completed, auto total) {
                             EXPECT_EQ(total, 40U);
                             done = completed;
                             if (completed == 10)
                               stop.request_stop();
                           }),
               std::runtime_error);
  EXPECT_EQ(done, 10U);
}
TEST(Factor, ConstantPricesProduceMissingStatisticsNotZero) {
  auto value = input();
  for (auto& t : *value.mutable_dataset()->mutable_bars())
    t.mutable_close()->set_units(d("100").raw());
  revision(value);
  const auto result = factor::run(value);
  EXPECT_FALSE(result.partitions(0).has_pearson());
  EXPECT_FALSE(result.partitions(0).has_spearman());
  const auto decoded = protocol::decode_factor_result(result);
  EXPECT_TRUE(decoded.at("partitions").at(0).at("pearson").is_null());
  EXPECT_TRUE(decoded.at("partitions").at(0).at("spearman").is_null());
}
#include "task_store.hpp"
namespace {
namespace wire = task::v1;
struct FactorTasks : testing::Test {
  std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("ast-factor-task-" + unique_process_id());
  void SetUp() override { std::filesystem::create_directory(root); }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};
} // namespace
TEST_F(FactorTasks, ResultAcceptanceChecksIdentityAndTimelineWithoutReexecutingAlgorithm) {
  auto spec = input();
  for (auto& bar : *spec.mutable_dataset()->mutable_bars())
    bar.mutable_close()->set_units(d("100").raw());
  revision(spec);
  auto result = factor::run(spec);
  ASSERT_FALSE(result.partitions(0).has_pearson());
  ASSERT_FALSE(result.partitions(0).has_spearman());
  ASSERT_EQ(result.samples(0).value(), 0.0);
  result.mutable_samples(0)->set_value(-0.0);
  EXPECT_NO_THROW(protocol::validate_factor_result(spec, result));
  auto wrong_order = result;
  wrong_order.mutable_samples()->SwapElements(0, 1);
  EXPECT_THROW(protocol::validate_factor_result(spec, wrong_order), std::invalid_argument);
  spec = input();
  result = factor::run(spec);
  result.mutable_samples(0)->set_value(std::nextafter(result.samples(0).value(), 1.0));
  EXPECT_NO_THROW(protocol::validate_factor_result(spec, result));
  {
    tasks::Store store(root, tasks::Identity{"factor-tests", "fixture-data"});
    tasks::submit(store, "accepted", spec);
    store.commit(
        tasks::finish(store, "accepted", store.commit(store.claim("accepted")).token(), result));
  }
  tasks::Store restored(root, tasks::Identity{"factor-tests", "fixture-data"});
  EXPECT_EQ(tasks::factor_result(restored, "accepted").SerializeAsString(),
            result.SerializeAsString());
}

TEST_F(FactorTasks, DurableTypeIdentityCancellationAndTamperedResult) {
  asterion::factor::v1::FactorResult expected = factor::run(input());
  {
    tasks::Store store(root, tasks::Identity{"factor-tests", "fixture-data"});
    auto task = tasks::submit(store, "factor", input());
    wire::TaskDispatch allowance;
    allowance.set_launch_slots(1);
    const auto launches = store.dispatch(allowance);
    ASSERT_EQ(launches.launches_size(), 1);
    EXPECT_EQ(launches.launches(0).program(), wire::FACTOR_PROGRAM);

    EXPECT_EQ(task.kind(), wire::FACTOR);
    EXPECT_EQ(tasks::submit(store, "factor", input()).id(), task.id());
    EXPECT_FALSE(store.list().tasks(0).has_factor());
    auto changed = input();
    changed.set_horizon(2);
    EXPECT_THROW(tasks::submit(store, "factor", changed), std::invalid_argument);
    auto token = store.commit(store.claim("factor")).token();
    auto bad = expected;
    bad.set_dataset_revision("wrong-input");
    EXPECT_THROW(store.commit(tasks::finish(store, "factor", token, bad)), std::invalid_argument);
    EXPECT_EQ(store.get("factor").state(), wire::RUNNING);
    store.commit(store.cancel("factor")).task();
    store.commit(tasks::finish(store, "factor", token, expected));
    EXPECT_EQ(store.get("factor").state(), wire::CANCELLED);
    store.commit(store.retry("factor")).task();
    const auto next = store.commit(store.claim("factor")).token();
    EXPECT_NE(next, token);
    EXPECT_THROW(store.commit(tasks::finish(store, "factor", token, expected)),
                 std::invalid_argument);
    store.commit(tasks::finish(store, "factor", next, expected));
    EXPECT_THROW(tasks::result(store, "factor"), std::invalid_argument);
  }
  {
    tasks::Store recovered(root, tasks::Identity{"factor-tests", "fixture-data"});
    EXPECT_EQ(tasks::factor_result(recovered, "factor").SerializeAsString(),
              expected.SerializeAsString());
    EXPECT_EQ(recovered.get("factor").attempt(), 2U);
  }
}

namespace {
factor::v1::FactorInput holdout_input() {
  auto result = input();
  result.mutable_dataset()->clear_bars();
  for (int i = 0; i < 100; ++i) {
    auto* t = result.mutable_dataset()->add_bars();
    t->set_timestamp_ns(1790298000000000000LL + static_cast<std::int64_t>(i) * 1000000000);
    t->mutable_close()->set_units(Decimal::parse(std::to_string(100 + i + i % 7)).raw());
    t->mutable_volume()->set_units(d("1").raw());
  }
  result.set_horizon(3);
  result.set_holdout_start(50);
  revision(result);
  return result;
}
} // namespace
TEST(Factor, HoldoutPurgesBoundaryLabelsAndReportsIndependentStatistics) {
  const auto value = holdout_input();
  const auto result = factor::run(value);
  ASSERT_EQ(result.partitions_size(), 2);
  EXPECT_EQ(result.purged_count(), 3U);
  EXPECT_EQ(result.samples_size(), 92);
  const auto& development = result.partitions(0);
  const auto& holdout = result.partitions(1);
  EXPECT_EQ(development.name(), "development");
  EXPECT_EQ(development.begin_index(), 0U);
  EXPECT_EQ(development.end_index(), 50U);
  EXPECT_EQ(development.sample_count(), 45U);
  EXPECT_EQ(holdout.name(), "holdout");
  EXPECT_EQ(holdout.begin_index(), 50U);
  EXPECT_EQ(holdout.end_index(), 100U);
  EXPECT_EQ(holdout.sample_count(), 47U);
  std::vector<double> features, labels;
  for (const auto& s : result.samples()) {
    if (s.event_index() < 50) {
      EXPECT_LT(s.label_timestamp_ns(), value.dataset().bars(50).timestamp_ns());
    } else {
      features.push_back(s.value());
      labels.push_back(s.forward_return());
    }
  }
  EXPECT_DOUBLE_EQ(holdout.pearson(), *correlation(features, labels));
  EXPECT_DOUBLE_EQ(holdout.spearman(), *rank_correlation(features, labels));
  auto modified = value;
  for (int i = 50; i < 100; ++i)
    modified.mutable_dataset()->mutable_bars(i)->mutable_close()->set_units(d("500").raw());
  revision(modified);
  const auto changed = factor::run(modified);
  EXPECT_EQ(development.SerializeAsString(), changed.partitions(0).SerializeAsString());
  for (int i = 0; i < 45; ++i)
    EXPECT_EQ(result.samples(i).SerializeAsString(), changed.samples(i).SerializeAsString());
  // Historical warmup is retained; the holdout does not restart at zero
  // history.
  EXPECT_EQ(result.samples(45).event_index(), 50U);
}
TEST(Factor, ExplicitEvaluationAndSplitValidationRejectMissingOrLeakingInputs) {
  auto value = holdout_input();
  const auto digest = value.dataset_revision();
  value.set_holdout_start(51);
  EXPECT_EQ(protocol::bar_dataset_revision(value.dataset()), digest);
  value.clear_evaluation();
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  value.set_full_sample(false);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  for (unsigned split : {0U, 34U, 68U, 100U, 10000U}) {
    value.set_holdout_start(split);
    EXPECT_THROW(factor::run(value), std::invalid_argument);
  }
  value = holdout_input();
  value.mutable_dataset()->mutable_bars(50)->set_timestamp_ns(
      value.dataset().bars(49).timestamp_ns());
  EXPECT_THROW(revision(value), std::invalid_argument);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
}
TEST_F(FactorTasks, HoldoutPartitionIdentityTamperingAndRecovery) {
  const auto value = holdout_input();
  const auto expected = factor::run(value);
  {
    tasks::Store store(root, tasks::Identity{"factor-tests", "fixture-data"});
    tasks::submit(store, "holdout", value);
    auto changed = value;
    changed.set_holdout_start(51);
    EXPECT_THROW(tasks::submit(store, "holdout", changed), std::invalid_argument);
    const auto token = store.commit(store.claim("holdout")).token();
    auto bad = expected;
    bad.mutable_partitions(1)->set_sample_count(48);
    EXPECT_THROW(store.commit(tasks::finish(store, "holdout", token, bad)), std::invalid_argument);
    bad = expected;
    bad.set_purged_count(0);
    EXPECT_THROW(store.commit(tasks::finish(store, "holdout", token, bad)), std::invalid_argument);
    store.commit(tasks::finish(store, "holdout", token, expected));
  }
  tasks::Store recovered(root, tasks::Identity{"factor-tests", "fixture-data"});
  EXPECT_EQ(tasks::factor_result(recovered, "holdout").SerializeAsString(),
            expected.SerializeAsString());
}

namespace {
factor::v1::FactorInput search_input() {
  auto value = holdout_input();
  value.add_lookbacks(5);
  value.add_lookbacks(10);
  return value;
}
} // namespace
TEST(Factor, CandidatePolicyUsesAbsoluteDevelopmentScoreAndDeterministicTies) {
  std::vector<MomentumCandidateScore> candidates{{2, .2}, {5, -.8}, {10, .8}};
  EXPECT_EQ(select_momentum_lookback(candidates), 5U);
  candidates = {{2, std::nullopt}, {5, 0.0}};
  EXPECT_EQ(select_momentum_lookback(candidates), 5U);
  candidates = {{2, std::nullopt}, {5, std::nullopt}};
  EXPECT_FALSE(select_momentum_lookback(candidates));
  candidates = {{5, .2}, {2, .8}};
  EXPECT_THROW(select_momentum_lookback(candidates), std::invalid_argument);
  candidates = {{2, std::numeric_limits<double>::quiet_NaN()}};
  EXPECT_THROW(select_momentum_lookback(candidates), std::invalid_argument);
}
TEST(Factor, SearchUsesCommonSamplesAndNeverSelectsAgainstHoldout) {
  auto value = search_input();
  std::size_t progress = 0;
  const auto result = factor::run(value, {}, [&](auto done, auto total) {
    EXPECT_EQ(total, 250U);
    EXPECT_EQ(done, ++progress);
  });
  EXPECT_EQ(progress, 250U);
  EXPECT_EQ(result.evaluation_warmup(), 10U);
  EXPECT_EQ(result.selection_rule(), "development_abs_spearman");
  EXPECT_EQ(result.partitions(0).sample_count(), 37U);
  EXPECT_EQ(result.partitions(1).sample_count(), 47U);
  ASSERT_EQ(result.candidates_size(), 3);
  for (const auto& candidate : result.candidates()) {
    EXPECT_EQ(candidate.sample_count(), 37U);
    std::vector<double> factors, labels;
    for (int i = 10; i < 47; ++i) {
      auto price = [&](int index) {
        return Decimal::from_raw(value.dataset().bars(index).close().units());
      };
      factors.push_back(price_return(price(i - static_cast<int>(candidate.lookback())), price(i)));
      labels.push_back(price_return(price(i), price(i + 3)));
    }
    EXPECT_DOUBLE_EQ(candidate.development_spearman(), *rank_correlation(factors, labels));
    if (candidate.lookback() == result.lookback())
      EXPECT_DOUBLE_EQ(candidate.development_spearman(), result.partitions(0).spearman());
  }
  for (int i = 50; i < 100; ++i)
    value.mutable_dataset()->mutable_bars(i)->mutable_close()->set_units(
        Decimal::parse(std::to_string(1000 - i * 3)).raw());
  revision(value);
  const auto changed = factor::run(value);
  EXPECT_EQ(result.lookback(), changed.lookback());
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(result.candidates(i).SerializeAsString(), changed.candidates(i).SerializeAsString());
  EXPECT_EQ(result.partitions(0).SerializeAsString(), changed.partitions(0).SerializeAsString());
}
TEST(Factor, SearchRejectsInvalidCandidateSetsAndCancelsDuringSelection) {
  auto value = search_input();
  value.set_full_sample(true);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  value = search_input();
  value.set_lookbacks(1, 2);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  value.clear_lookbacks();
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  for (unsigned i = 1; i <= 33; ++i)
    value.add_lookbacks(i);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  value = search_input();
  for (auto& t : *value.mutable_dataset()->mutable_bars())
    t.mutable_close()->set_units(d("100").raw());
  revision(value);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  value.clear_lookbacks();
  value.add_lookbacks(2);
  EXPECT_NO_THROW(factor::run(value));
  std::stop_source stop;
  unsigned progress = 0;
  EXPECT_THROW(factor::run(search_input(), stop.get_token(),
                           [&](auto done, auto) {
                             progress = static_cast<unsigned>(done);
                             if (done == 55)
                               stop.request_stop();
                           }),
               std::runtime_error);
  EXPECT_EQ(progress, 55U);
}
TEST_F(FactorTasks, SearchSelectionAndCandidateEvidencePersistAndRejectTampering) {
  const auto value = search_input();
  const auto expected = factor::run(value);
  {
    tasks::Store store(root, tasks::Identity{"factor-tests", "fixture-data"});
    EXPECT_EQ(tasks::submit(store, "search", value).total(), 250U);
    const auto token = store.commit(store.claim("search")).token();
    auto bad = expected;
    bad.mutable_candidates(0)->set_lookback(999);
    EXPECT_THROW(store.commit(tasks::finish(store, "search", token, bad)), std::invalid_argument);
    bad = expected;
    bad.set_lookback(99);
    EXPECT_THROW(store.commit(tasks::finish(store, "search", token, bad)), std::invalid_argument);
    store.commit(tasks::finish(store, "search", token, expected));
  }
  tasks::Store recovered(root, tasks::Identity{"factor-tests", "fixture-data"});
  EXPECT_EQ(tasks::factor_result(recovered, "search").SerializeAsString(),
            expected.SerializeAsString());
}

TEST_F(FactorTasks, ResultEvidencePreservesAllCandidatesAndHoldoutConfiguration) {
  const auto spec = search_input();
  tasks::Store store(root, tasks::Identity{"factor-tests", "fixture-data"});
  tasks::submit(store, "factor", spec);
  store.commit(tasks::finish(store, "factor", store.commit(store.claim("factor")).token(),
                             factor::run(spec)));
  task::v1::TaskResponse response;
  *response.mutable_result_task() = store.get("factor");
  *response.mutable_factor() = tasks::factor_result(store, "factor");
  const auto evidence = protocol::decode_task_result(response, "factor");
  EXPECT_EQ(evidence.at("experiment").at("lookbacks").size(),
            static_cast<std::size_t>(spec.lookbacks_size()));
  for (int i = 0; i < spec.lookbacks_size(); ++i)
    EXPECT_EQ(evidence.at("experiment").at("lookbacks").at(i), spec.lookbacks(i));
  EXPECT_EQ(evidence.at("experiment").at("evaluation").at("mode"), "holdout");
  EXPECT_EQ(evidence.at("experiment").at("evaluation").at("split_index"), spec.holdout_start());
  EXPECT_EQ(evidence.at("experiment").at("data").at("count"), spec.dataset().bars_size());
  EXPECT_FALSE(evidence.at("experiment").contains("ticks"));
  EXPECT_EQ(evidence.at("experiment").at("dataset_revision"), response.factor().dataset_revision());
}

namespace {
factor::v1::FactorInput rolling_input() {
  auto value = search_input();
  value.mutable_dataset()->clear_bars();
  for (int i = 0; i < 160; ++i) {
    auto* row = value.mutable_dataset()->add_bars();
    row->set_timestamp_ns(1790298000000000000LL + std::int64_t{i} * 1000000000);
    row->mutable_close()->set_units(d(std::to_string(100 + i + (i * i % 17)).c_str()).raw());
    row->mutable_volume()->set_units(d("1").raw());
  }
  value.mutable_walk_forward()->set_training_events(80);
  value.mutable_walk_forward()->set_validation_events(40);
  revision(value);
  return value;
}
} // namespace
TEST(Factor, RollingValidationCoversWindowsAndPurgesLabelsWithMonotoneProgress) {
  const auto value = rolling_input();
  unsigned previous = 0;
  const auto result = factor::run(value, {}, [&](auto done, auto total) {
    EXPECT_EQ(total, 720U);
    EXPECT_EQ(done, ++previous);
  });
  EXPECT_EQ(previous, 720U);
  ASSERT_EQ(result.folds_size(), 2);
  EXPECT_EQ(result.samples_size(), 74);
  EXPECT_EQ(result.purged_count(), 6U);
  EXPECT_EQ(result.lookback(), 0U); // No invented global selected parameter.
  EXPECT_EQ(result.folds(0).training_begin(), 0U);
  EXPECT_EQ(result.folds(0).training_end(), 80U);
  EXPECT_EQ(result.folds(0).validation_end(), 120U);
  EXPECT_EQ(result.folds(1).training_begin(), 40U);
  EXPECT_EQ(result.folds(1).training_end(), 120U);
  EXPECT_EQ(result.folds(1).validation_end(), 160U);
  for (const auto& fold : result.folds()) {
    EXPECT_EQ(fold.development().sample_count(), 67U);
    EXPECT_EQ(fold.holdout().sample_count(), 37U);
    for (const auto& sample : result.samples()) {
      if (sample.event_index() < fold.training_end() ||
          sample.event_index() >= fold.validation_end())
        continue;
      EXPECT_LT(sample.event_index() + value.horizon(), fold.validation_end());
      EXPECT_LT(sample.label_timestamp_ns(),
                value.dataset().bars(fold.validation_end() - 1).timestamp_ns() + 1);
    }
  }
  const auto decoded = protocol::decode_factor_result(result);
  EXPECT_EQ(decoded.at("folds").size(), 2U);
  EXPECT_TRUE(decoded.at("partitions").empty());
}
TEST(Factor, RollingSelectionNeverReadsValidationOrLaterFolds) {
  auto original = rolling_input();
  const auto baseline = factor::run(original);
  auto changed = original;
  for (int i = 80; i < changed.dataset().bars_size(); ++i)
    changed.mutable_dataset()->mutable_bars(i)->mutable_close()->set_units(
        d(std::to_string(900 + i * i % 31).c_str()).raw());
  revision(changed);
  const auto second = factor::run(changed);
  EXPECT_EQ(second.folds(0).lookback(), baseline.folds(0).lookback());
  EXPECT_EQ(second.folds(0).development().SerializeAsString(),
            baseline.folds(0).development().SerializeAsString());
  for (int i = 0; i < baseline.folds(0).candidates_size(); ++i)
    EXPECT_EQ(second.folds(0).candidates(i).SerializeAsString(),
              baseline.folds(0).candidates(i).SerializeAsString());
  changed = original;
  for (int i = 120; i < changed.dataset().bars_size(); ++i)
    changed.mutable_dataset()->mutable_bars(i)->mutable_close()->set_units(
        d(std::to_string(800 + i % 13).c_str()).raw());
  revision(changed);
  const auto third = factor::run(changed);
  EXPECT_EQ(third.folds(0).SerializeAsString(), baseline.folds(0).SerializeAsString());
  EXPECT_EQ(third.folds(1).lookback(), baseline.folds(1).lookback());
  for (int i = 0; i < 37; ++i)
    EXPECT_EQ(third.samples(i).SerializeAsString(), baseline.samples(i).SerializeAsString());
}
TEST(Factor, RollingRejectsPartialWindowsTimestampSplitsAndHonoursCancellation) {
  auto value = rolling_input();
  value.mutable_walk_forward()->set_validation_events(39);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  value = rolling_input();
  value.mutable_walk_forward()->set_training_events(120);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  value = rolling_input();
  value.mutable_dataset()->mutable_bars(80)->set_timestamp_ns(
      value.dataset().bars(79).timestamp_ns());
  EXPECT_THROW(revision(value), std::invalid_argument);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  value = rolling_input();
  value.mutable_dataset()->mutable_bars(40)->set_timestamp_ns(
      value.dataset().bars(39).timestamp_ns());
  EXPECT_THROW(revision(value), std::invalid_argument);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  value = rolling_input();
  value.set_version(3);
  EXPECT_THROW(factor::run(value), std::invalid_argument);
  std::stop_source stop;
  EXPECT_THROW(factor::run(rolling_input(), stop.get_token(),
                           [&](auto done, auto) {
                             if (done == 365)
                               stop.request_stop();
                           }),
               std::runtime_error);
}
TEST_F(FactorTasks, RollingEvidencePersistsAndRejectsChangedWindowsOrInvalidScores) {
  const auto value = rolling_input();
  const auto expected = factor::run(value);
  {
    tasks::Store store(root, tasks::Identity{"factor-tests", "fixture-data"});
    EXPECT_EQ(tasks::submit(store, "rolling", value).total(), 720U);
    const auto token = store.commit(store.claim("rolling")).token();
    auto bad = expected;
    bad.mutable_folds(1)->set_training_begin(0);
    EXPECT_THROW(store.commit(tasks::finish(store, "rolling", token, bad)), std::invalid_argument);
    bad = expected;
    bad.mutable_folds(0)->mutable_holdout()->set_spearman(1.5);
    EXPECT_THROW(store.commit(tasks::finish(store, "rolling", token, bad)), std::invalid_argument);
    store.commit(tasks::finish(store, "rolling", token, expected));
  }
  tasks::Store recovered(root, tasks::Identity{"factor-tests", "fixture-data"});
  EXPECT_EQ(tasks::factor_result(recovered, "rolling").SerializeAsString(),
            expected.SerializeAsString());
  task::v1::TaskResponse response;
  *response.mutable_result_task() = recovered.get("rolling");
  *response.mutable_factor() = tasks::factor_result(recovered, "rolling");
  const auto evidence = protocol::decode_task_result(response, "rolling");
  EXPECT_EQ(evidence.at("experiment").at("evaluation").at("training_events"), 80);
  EXPECT_EQ(evidence.at("result").at("folds").size(), 2U);
}

TEST(Factor, PriceObservationsShareMomentumWithoutInventingTradeFields) {
  PriceMomentum window(2);
  EXPECT_FALSE(window.push(d("100.00000001")));
  EXPECT_FALSE(window.push(d("105.00000001")));
  EXPECT_THROW(window.push(d("0")), std::invalid_argument);
  EXPECT_THROW(window.push(d("-1")), std::invalid_argument);
  EXPECT_DOUBLE_EQ(*window.push(d("110.00000001")),
                   price_return(d("100.00000001"), d("110.00000001")));
  EXPECT_DOUBLE_EQ(*window.push(d("120.00000001")),
                   price_return(d("105.00000001"), d("120.00000001")));
  window.reset();
  EXPECT_FALSE(window.push(d("1")));
  EXPECT_THROW(PriceMomentum(0), std::invalid_argument);
  EXPECT_THROW(PriceMomentum(10001), std::invalid_argument);
  MomentumFactor trades(spec(), 2);
  PriceMomentum prices(2);
  trades.start();
  for (int i = 0; i < 100; ++i) {
    const auto price = Decimal::parse(std::to_string(100 + i + i % 3));
    EXPECT_EQ(prices.push(price),
              trades.on_bar({"2026-09-25", i, price, price, price, price, d("1")}));
  }
}

TEST(Factor, DailyMomentumPreservesDatesAndNeverFeedsFutureClosesIntoFeatures) {
  using namespace std::chrono;
  std::vector<HistoricalDailyBar> bars;
  for (int i = 0; i < 8; ++i) {
    const auto price = Decimal::parse(std::to_string(100 + 10 * i));
    bars.push_back({year_month_day{sys_days{year{2024} / January / 1} + days{2 * i}},
                    price,
                    price,
                    price,
                    price,
                    d("1"),
                    d("1"),
                    d("1"),
                    {},
                    {},
                    {}});
  }
  std::size_t completed = 0;
  const auto all = daily_momentum_samples(bars, 2, 2, {}, [&](auto done, auto total) {
    EXPECT_EQ(done, ++completed);
    EXPECT_EQ(total, bars.size());
  });
  ASSERT_EQ(all.size(), 4);
  EXPECT_EQ(all.front().observation_index, 2);
  EXPECT_EQ(format_trading_date(all.front().trading_day), "2024-01-05");
  EXPECT_EQ(format_trading_date(all.front().label_day), "2024-01-09");
  EXPECT_DOUBLE_EQ(all.front().value, price_return(d("100"), d("120")));
  EXPECT_DOUBLE_EQ(all.front().forward_return, price_return(d("120"), d("140")));
  bars[4].open = bars[4].high = bars[4].low = bars[4].close = d("200");
  const auto changed = daily_momentum_samples(bars, 2, 2);
  EXPECT_DOUBLE_EQ(changed.front().value, all.front().value);
  EXPECT_NE(changed.front().forward_return, all.front().forward_return);
  EXPECT_THROW(daily_momentum_samples(bars, 0, 2), std::invalid_argument);
  EXPECT_THROW(daily_momentum_samples(bars, 2, 0), std::invalid_argument);
  EXPECT_THROW(daily_momentum_samples(bars, 7, 2), std::invalid_argument);
  auto bad = bars;
  bad[3].trading_day = bad[2].trading_day;
  EXPECT_THROW(daily_momentum_samples(bad, 2, 2), std::invalid_argument);
  bad[3].trading_day = bad[1].trading_day;
  EXPECT_THROW(daily_momentum_samples(bad, 2, 2), std::invalid_argument);
  std::stop_source stop;
  EXPECT_THROW(daily_momentum_samples(bars, 2, 2, stop.get_token(),
                                      [&](auto done, auto) {
                                        if (done == 3)
                                          stop.request_stop();
                                      }),
               std::runtime_error);
  EXPECT_THROW(daily_momentum_samples(bars, 2, 2, stop.get_token()), std::runtime_error);
}

TEST(Factor, DailyInputBindsSourceAndExactBarsAndPurgesHoldoutLabels) {
  using namespace std::chrono;
  factor::v1::DailyFactorInput input;
  input.set_version(1);
  input.set_lookback(2);
  input.set_horizon(2);
  input.set_holdout_start(40);
  auto& dataset = *input.mutable_dataset();
  dataset.set_version(2);
  dataset.mutable_history_evidence()->set_dataset_id(std::string(64, 'a'));
  dataset.mutable_history_evidence()->set_acquired_at_ns(1790000000000000000);
  dataset.mutable_history_evidence()->set_source_availability(
      data::v1::SOURCE_AVAILABILITY_UNKNOWN);
  dataset.set_source_dataset_id(std::string(64, 'a'));
  dataset.set_source("tushare.fut_daily");
  dataset.set_contract_id("SHFE/cu/2024-03");
  dataset.set_manifest_sha256(std::string(64, 'a'));
  for (int i = 0; i < 80; ++i) {
    auto* bar = dataset.add_bars();
    bar->set_trading_day(
        format_trading_date(year_month_day{sys_days{year{2023} / January / 1} + days{i}}));
    const auto price = Decimal::parse(std::to_string(100 + i + i % 3));
    for (auto* value :
         {bar->mutable_open(), bar->mutable_high(), bar->mutable_low(), bar->mutable_close()})
      value->set_units(price.raw());
    bar->mutable_volume()->set_units(d("1").raw());
    bar->mutable_amount()->set_units(d("100.00000001").raw());
    bar->mutable_open_interest()->set_units(d("3").raw());
  }
  input.set_dataset_revision(protocol::daily_factor_revision(dataset));
  const auto result = factor::run_daily(input);
  EXPECT_EQ(result.input_count(), 80);
  EXPECT_EQ(result.purged_count(), 2);
  ASSERT_EQ(result.partitions_size(), 2);
  EXPECT_EQ(result.partitions(0).sample_count(), 36);
  EXPECT_EQ(result.partitions(1).sample_count(), 38);
  EXPECT_EQ(result.samples(0).trading_day(), "2023-01-03");
  EXPECT_EQ(result.samples(0).label_day(), "2023-01-05");
  for (const auto& row : result.samples())
    EXPECT_TRUE(row.observation_index() >= 40 || row.observation_index() + 2 < 40);
  const auto decoded = protocol::decode_daily_factor(input, result);
  EXPECT_EQ(decoded.at("experiment").at("data").at("count"), 80);
  EXPECT_EQ(decoded.at("experiment").at("data").at("first_day"), "2023-01-01");
  EXPECT_EQ(decoded.at("result").at("samples").size(), 74);
  auto invalid_evidence = result;
  invalid_evidence.mutable_samples(0)->set_label_day("2023-01-04");
  EXPECT_THROW(protocol::decode_daily_factor(input, invalid_evidence), std::invalid_argument);
  invalid_evidence = result;
  invalid_evidence.mutable_samples(0)->set_value(std::numeric_limits<double>::infinity());
  EXPECT_THROW(protocol::decode_daily_factor(input, invalid_evidence), std::invalid_argument);
  invalid_evidence = result;
  invalid_evidence.mutable_partitions(0)->set_sample_count(99);
  EXPECT_THROW(protocol::decode_daily_factor(input, invalid_evidence), std::invalid_argument);
  invalid_evidence = result;
  invalid_evidence.set_purged_count(0);
  EXPECT_THROW(protocol::decode_daily_factor(input, invalid_evidence), std::invalid_argument);
  Json parameters = {{"source_dataset_id", std::string(64, 'a')},
                     {"lookback", 2},
                     {"horizon", 2},
                     {"evaluation", {{"mode", "holdout"}, {"split_index", 40}}}};
  EXPECT_EQ(protocol::encode_daily_factor_request(parameters).holdout_start(), 40);
  for (const Json mutation :
       {Json{{"lookback", 2.5}}, Json{{"horizon", 0}}, Json{{"lookback", -1}},
        Json{{"evaluation", {{"mode", "walk_forward"}}}}, Json{{"bars", Json::array()}}}) {
    auto invalid = parameters;
    invalid.update(mutation);
    EXPECT_THROW(protocol::encode_daily_factor_request(invalid), std::exception);
  }
  EXPECT_NO_THROW((void)protocol::decode_daily_factor(input, result));
  auto wrong = result;
  wrong.mutable_samples(0)->set_label_day("2023-01-04");
  EXPECT_THROW((void)protocol::decode_daily_factor(input, wrong), std::invalid_argument);
  auto changed = input;
  changed.set_lookback(3);
  EXPECT_EQ(protocol::daily_factor_revision(changed.dataset()), input.dataset_revision());
  changed.mutable_dataset()->mutable_bars(0)->mutable_amount()->set_units(d("100.00000002").raw());
  EXPECT_NE(protocol::daily_factor_revision(changed.dataset()), input.dataset_revision());
  EXPECT_THROW(factor::run_daily(changed), std::invalid_argument);
  changed = input;
  changed.mutable_dataset()->set_source_dataset_id("another-source");
  EXPECT_THROW(factor::run_daily(changed), std::invalid_argument);
  changed = input;
  changed.mutable_dataset()->mutable_bars(0)->clear_volume();
  EXPECT_THROW(factor::run_daily(changed), std::invalid_argument);
  input.set_full_sample(true);
  EXPECT_EQ(factor::run_daily(input).samples_size(), 76);
  for (auto& bar : *input.mutable_dataset()->mutable_bars())
    for (auto* value :
         {bar.mutable_open(), bar.mutable_high(), bar.mutable_low(), bar.mutable_close()})
      value->set_units(d("100").raw());
  input.set_dataset_revision(protocol::daily_factor_revision(input.dataset()));
  const auto constant = factor::run_daily(input);
  EXPECT_FALSE(constant.partitions(0).has_pearson());
  EXPECT_FALSE(constant.partitions(0).has_spearman());
  EXPECT_TRUE(protocol::decode_daily_factor(input, constant)
                  .at("result")
                  .at("partitions")
                  .at(0)
                  .at("pearson")
                  .is_null());
}
