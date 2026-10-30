#pragma once
#include <asterion/foundation/decimal.hpp>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace asterion {

enum class FeatureUndefined
{
    none,
    warmup,
    zero_denominator,
    non_finite
};

struct FeatureValue
{
    std::optional<double> value;
    FeatureUndefined reason = FeatureUndefined::warmup;
};

class FeatureExpressionError : public std::invalid_argument
{
public:
    FeatureExpressionError(std::size_t position, std::string reason);

    std::size_t position() const noexcept
    {
        return position_;
    }

    const std::string& reason() const noexcept
    {
        return reason_;
    }

private:
    std::size_t position_;
    std::string reason_;
};

// Trusted analysis algorithm. Its grammar has no scripts, external I/O,
// future fields or negative lags. Values are research quantities, not money.
class FeatureExpression
{
public:
    explicit FeatureExpression(std::string source);
    const std::string& source() const noexcept;
    int price_power() const noexcept;
    std::size_t history(std::size_t window) const;
    std::vector<FeatureValue>
    evaluate(std::span<const Decimal>, std::size_t window, std::stop_token = {}) const;

private:
    struct Impl;
    std::shared_ptr<const Impl> program_;
};

} // namespace asterion
