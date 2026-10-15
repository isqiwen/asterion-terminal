#pragma once
#include <asterion/domain/history_identity.hpp>
#include <asterion/domain/trading_days.hpp>
#include <asterion/foundation/decimal.hpp>
#include <optional>
#include <stdexcept>
#include <string>
namespace asterion::historical_costs {
enum class MarginUnit { unknown, fraction, percent };
enum class MarginPhase { unknown, trading_day_start, settlement };
enum class MarginBasis { unknown, exchange_speculation, broker_actual };
// These are reviewed provider definitions, not assertions accepted from a UI/import file.
struct MarginDefinition {
  std::string source, exchange;
  MarginUnit unit = MarginUnit::unknown;
  MarginPhase phase = MarginPhase::unknown;
  MarginBasis basis = MarginBasis::unknown;
  std::string unit_reference, effective_reference, basis_reference;
};
struct MarginFact {
  std::string contract, observed_day, source;
  std::string snapshot, revision_snapshot, content, object, provider_artifact;
  std::int64_t acquired_at_ns = 0;
  std::optional<Decimal> long_rate, short_rate;
};
// Owns all facts and reviewed semantics needed to inspect this preparation.
// It is not yet a Task input or a source publication-time assertion.
struct PinnedMargin {
  MarginFact fact;
  MarginDefinition definition;
  std::string effective_day;
  Decimal long_rate, short_rate;
  std::optional<std::int64_t> published_at_ns;
  std::string publication_reference;
};
inline PinnedMargin pin_margin(const MarginFact& fact, const MarginDefinition& definition,
                               const std::string& effective_day) {
  const auto identity = HistoryIdentity::parse(fact.contract);
  (void)parse_trading_date(fact.observed_day);
  const auto hash = [](const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
  };
  if (fact.source.empty() || fact.source != definition.source ||
      definition.exchange != identity.venue || fact.acquired_at_ns <= 0 || !hash(fact.snapshot) ||
      !hash(fact.revision_snapshot) || !hash(fact.content) || !hash(fact.object) ||
      !hash(fact.provider_artifact))
    throw std::invalid_argument("historical margin source evidence is missing");
  if (!fact.long_rate || !fact.short_rate)
    throw std::invalid_argument("historical margin side value is missing");
  if (definition.unit == MarginUnit::unknown)
    throw std::invalid_argument("historical margin unit evidence is missing");
  if (definition.phase == MarginPhase::unknown || effective_day.empty())
    throw std::invalid_argument("historical margin effective-time evidence is missing");
  (void)parse_trading_date(effective_day);
  if (definition.basis == MarginBasis::unknown || definition.unit_reference.empty() ||
      definition.effective_reference.empty() || definition.basis_reference.empty())
    throw std::invalid_argument("historical margin semantic reference is missing");
  const auto normalize = [&](Decimal value) {
    const auto one = Decimal::parse("1");
    if (value <= Decimal{} ||
        value >= (definition.unit == MarginUnit::percent ? Decimal::parse("100") : one))
      throw std::invalid_argument("historical margin rate is invalid");
    if (definition.unit == MarginUnit::fraction)
      return value;
    // Decimal holds eight places. Do not silently round an unrepresentable percentage.
    if (value.raw() % 100 != 0)
      throw std::invalid_argument("historical margin conversion loses precision");
    return Decimal::from_raw(value.raw() / 100);
  };
  return {fact, definition, effective_day, normalize(*fact.long_rate), normalize(*fact.short_rate),
          {},   {}};
}
inline Decimal trading_day_start_margin(const PinnedMargin& margin, MarginBasis requested_basis) {
  if (margin.definition.phase != MarginPhase::trading_day_start)
    throw std::invalid_argument("settlement margin cannot be applied at trading-day start");
  if (requested_basis == MarginBasis::unknown || margin.definition.basis != requested_basis)
    throw std::invalid_argument("exchange and broker margin bases must not be substituted");
  if (margin.long_rate != margin.short_rate)
    throw std::invalid_argument(
        "asymmetric margin cannot use the current single-rate cost schedule");
  return margin.long_rate;
}
inline void require_margin_known_by(const PinnedMargin& margin, std::int64_t event_ns) {
  if (!margin.published_at_ns || *margin.published_at_ns <= 0 ||
      margin.publication_reference.empty())
    throw std::invalid_argument("historical margin publication-time evidence is missing");
  if (*margin.published_at_ns > event_ns)
    throw std::invalid_argument("historical margin was not published by the requested event");
}
} // namespace asterion::historical_costs
