#pragma once
#include <asterion/base/decimal.hpp>
#include <asterion/strategy.pb.h>
#include <google/protobuf/message.h>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace asterion::trading {

std::string read_strategy_file(const std::filesystem::path& path, std::size_t limit);
std::string proto_to_json(const google::protobuf::Message& message);

void validate_package_identity(const std::string& name, const std::string& version);
void validate_strategy_package(const ::strategy::Package& package);
void validate_strategy_instance(const ::strategy::Instance& instance);

std::string warmup_digest(const ::strategy::Warmup& warmup);
std::string series_symbol(const ::strategy::DominantSeries& series);

// Annualized log roll yield 12 * ln(near / far) / delta_months, matching dominant.py.
std::optional<Decimal> annualized_roll_yield(
    Decimal near_price,
    Decimal far_price,
    const std::string& near_month,
    const std::string& far_month);
std::optional<Decimal> simple_return(Decimal current, Decimal previous);
bool candidate_expiring(
    const std::vector<std::string>& expiries,
    std::size_t index,
    const std::string& day);

} // namespace asterion::trading
