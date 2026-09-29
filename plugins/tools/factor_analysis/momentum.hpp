#pragma once
#include <asterion/domain/factor_port.hpp>
#include <deque>
#include <span>
namespace asterion {
double price_return(Decimal from, Decimal to);
// Undefined for constant series. Ranks use the average rank for ties.
std::optional<double> correlation(std::span<const double> x, std::span<const double> y);
std::optional<double> rank_correlation(std::span<const double> x, std::span<const double> y);
struct MomentumCandidateScore {
  unsigned lookback;
  std::optional<double> development_spearman;
};
// Fixed policy: maximum absolute development rank correlation; exact ties
// choose the smaller window. Undefined scores are excluded, never zero-filled.
std::optional<unsigned>
select_momentum_lookback(std::span<const MomentumCandidateScore> candidates);
// Ordered positive-price observations. The source adapter owns instrument,
// time and sampling semantics; this window never creates trade events.
class PriceMomentum {
public:
  explicit PriceMomentum(std::size_t lookback);
  void reset() noexcept;
  std::optional<double> push(Decimal price);

private:
  std::size_t lookback_;
  std::deque<Decimal> history_;
};
class MomentumFactor final : public FactorPort {
public:
  MomentumFactor(Instrument instrument, std::size_t lookback);
  PluginDescriptor descriptor() const override;
  void start() override;
  void stop() noexcept override;
  std::optional<double> on_tick(const TradeTick& tick) override;

private:
  Instrument instrument_;
  PriceMomentum prices_;
  std::int64_t last_time_ = -1;
  bool running_ = false;
};
} // namespace asterion
