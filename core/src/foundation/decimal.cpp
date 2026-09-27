#include <asterion/foundation/decimal.hpp>

#include <limits>
#include <stdexcept>

namespace asterion {
namespace {
constexpr std::uint64_t scale = 100000000;
constexpr auto max_value = std::numeric_limits<std::int64_t>::max();
constexpr auto min_value = std::numeric_limits<std::int64_t>::min();
std::uint64_t magnitude(std::int64_t value) {
  return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1
                   : static_cast<std::uint64_t>(value);
}
Decimal signed_value(std::uint64_t value, bool negative) {
  const auto limit = static_cast<std::uint64_t>(max_value) + (negative ? 1U : 0U);
  if (value > limit)
    throw std::overflow_error("decimal overflow");
  if (negative && value == limit)
    return Decimal::from_raw(min_value);
  const auto raw = static_cast<std::int64_t>(value);
  return Decimal::from_raw(negative ? -raw : raw);
}
std::uint64_t checked_multiply(std::uint64_t a, std::uint64_t b, std::uint64_t limit) {
  if (b != 0 && a > limit / b)
    throw std::overflow_error("decimal overflow");
  return a * b;
}
std::uint64_t checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t limit) {
  if (a > limit || b > limit - a)
    throw std::overflow_error("decimal overflow");
  return a + b;
}
} // namespace

Decimal Decimal::parse(std::string_view text) {
  if (text.empty())
    throw std::invalid_argument("empty decimal");
  const bool negative = text.front() == '-';
  if (negative)
    text.remove_prefix(1);
  if (text.empty())
    throw std::invalid_argument("decimal requires digits");
  const auto dot = text.find('.');
  const auto integer = text.substr(0, dot);
  const auto fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if (integer.empty() || (integer.size() > 1 && integer.front() == '0') ||
      (dot != std::string_view::npos && (fraction.empty() || fraction.size() > 8))) {
    throw std::invalid_argument("decimal requires canonical digits and at most eight places");
  }
  const auto limit = static_cast<std::uint64_t>(max_value) + (negative ? 1U : 0U);
  std::uint64_t value = 0;
  auto append = [&](char digit) {
    if (digit < '0' || digit > '9')
      throw std::invalid_argument("invalid decimal digit");
    value =
        checked_add(checked_multiply(value, 10, limit), static_cast<unsigned>(digit - '0'), limit);
  };
  for (const char digit : integer)
    append(digit);
  for (const char digit : fraction)
    append(digit);
  for (std::size_t i = fraction.size(); i < 8; ++i)
    append('0');
  return signed_value(value, negative);
}

std::string Decimal::str() const {
  const auto value = magnitude(raw_);
  auto result = (raw_ < 0 ? "-" : "") + std::to_string(value / scale);
  auto fraction = value % scale;
  if (fraction != 0) {
    auto tail = std::to_string(fraction + scale).substr(1);
    while (tail.back() == '0')
      tail.pop_back();
    result += "." + tail;
  }
  return result;
}

bool Decimal::multiple_of(Decimal increment) const {
  if (increment.raw_ <= 0)
    throw std::invalid_argument("increment must be positive");
  return raw_ % increment.raw_ == 0;
}

Decimal operator+(Decimal left, Decimal right) {
  if ((right.raw_ > 0 && left.raw_ > max_value - right.raw_) ||
      (right.raw_ < 0 && left.raw_ < min_value - right.raw_)) {
    throw std::overflow_error("decimal overflow");
  }
  return Decimal::from_raw(left.raw_ + right.raw_);
}

Decimal operator-(Decimal left, Decimal right) {
  if ((right.raw_ < 0 && left.raw_ > max_value + right.raw_) ||
      (right.raw_ > 0 && left.raw_ < min_value + right.raw_)) {
    throw std::overflow_error("decimal overflow");
  }
  return Decimal::from_raw(left.raw_ - right.raw_);
}

Decimal operator*(Decimal left, Decimal right) {
  const auto a = magnitude(left.raw_);
  const auto b = magnitude(right.raw_);
  const bool negative = (left.raw_ < 0) != (right.raw_ < 0);
  const auto limit = static_cast<std::uint64_t>(max_value) + (negative ? 1U : 0U);
  // Split the operands to avoid non-portable 128-bit integer extensions.
  const auto fraction = (a % scale) * (b % scale);
  if (fraction % scale != 0)
    throw std::domain_error("decimal multiplication would require rounding");
  auto value = checked_multiply(a / scale, b, limit);
  value = checked_add(value, checked_multiply(a % scale, b / scale, limit), limit);
  value = checked_add(value, fraction / scale, limit);
  return signed_value(value, negative);
}
namespace {
bool round_up(std::uint64_t remainder, std::uint64_t divisor, std::uint64_t quotient, bool negative,
              Rounding mode) {
  switch (mode) {
  case Rounding::exact:
    if (remainder)
      throw std::domain_error("decimal result would require rounding");
    return false;
  case Rounding::toward_zero:
    return false;
  case Rounding::floor:
    return remainder != 0 && negative;
  case Rounding::ceiling:
    return remainder != 0 && !negative;
  case Rounding::half_even:
    return remainder > divisor - remainder ||
           (remainder == divisor - remainder && quotient % 2 != 0);
  }
  throw std::invalid_argument("unknown decimal rounding mode");
}
} // namespace
Decimal divide(Decimal numerator, Decimal denominator, Rounding rounding) {
  const auto a = magnitude(numerator.raw());
  const auto b = magnitude(denominator.raw());
  if (!b)
    throw std::domain_error("decimal division by zero");
  const bool negative = (numerator.raw() < 0) != (denominator.raw() < 0);
  const auto limit = static_cast<std::uint64_t>(max_value) + (negative ? 1U : 0U);
  auto result = a / b;
  auto remainder = a % b;
  // Decimal long division without a widened integer or a binary float.
  for (unsigned i = 0; i < 8; ++i) {
    std::uint64_t accumulated = 0, digit = 0;
    for (unsigned j = 0; j < 10; ++j) {
      if (accumulated >= b - remainder) {
        accumulated -= b - remainder;
        ++digit;
      } else
        accumulated += remainder;
    }
    remainder = accumulated;
    result = checked_add(checked_multiply(result, 10, limit), digit, limit);
  }
  if (round_up(remainder, b, result, negative, rounding))
    result = checked_add(result, 1, limit);
  return signed_value(result, negative);
}
Decimal quantize(Decimal value, Decimal increment, Rounding rounding) {
  if (increment.raw() <= 0)
    throw std::invalid_argument("increment must be positive");
  const auto magnitude_value = magnitude(value.raw());
  const auto step = static_cast<std::uint64_t>(increment.raw());
  const bool negative = value.raw() < 0;
  const auto limit = static_cast<std::uint64_t>(max_value) + (negative ? 1U : 0U);
  auto quotient = magnitude_value / step;
  if (round_up(magnitude_value % step, step, quotient, negative, rounding))
    ++quotient;
  return signed_value(checked_multiply(quotient, step, limit), negative);
}
} // namespace asterion
