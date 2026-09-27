#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace asterion {

// Exact fixed-point value with eight decimal places. No implicit rounding.
// Supported range: -92233720368.54775808 to 92233720368.54775807.
enum class Rounding { exact, toward_zero, floor, ceiling, half_even };

class Decimal final {
public:
    constexpr Decimal() = default;
    static Decimal parse(std::string_view text);
    static constexpr Decimal from_raw(std::int64_t raw) { return Decimal(raw); }
    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }
    [[nodiscard]] std::string str() const;
    [[nodiscard]] bool multiple_of(Decimal increment) const;
    auto operator<=>(const Decimal&) const = default;
    friend Decimal operator+(Decimal left, Decimal right);
    friend Decimal operator-(Decimal left, Decimal right);
    friend Decimal operator*(Decimal left, Decimal right);

private:
    explicit constexpr Decimal(std::int64_t raw) : raw_(raw) {}
    std::int64_t raw_ = 0;
};

// Division and grid quantization require the caller to select any lossy rounding.
Decimal divide(Decimal numerator, Decimal denominator, Rounding rounding = Rounding::exact);
Decimal quantize(Decimal value, Decimal increment, Rounding rounding = Rounding::exact);

} // namespace asterion
