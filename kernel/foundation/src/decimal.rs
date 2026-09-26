//! Finite decimal arithmetic. Wire strings preserve scale, exponent and signed
//! zero; arithmetic rounds each operation using an explicit half-even context.
type Result<T> = std::result::Result<T, String>;
fn require(ok: bool, message: &str) -> Result<()> {
    if ok { Ok(()) } else { Err(message.into()) }
}
use bigdecimal::{BigDecimal, RoundingMode};
use num_bigint::BigInt;
use num_traits::{Signed, Zero};
use schemars::{JsonSchema, Schema, SchemaGenerator};
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use serde_json::{Value, json};
use std::{borrow::Cow, cmp::Ordering, fmt, str::FromStr};

const MAX_EXPONENT: i64 = 999_999;
const MAX_INPUT_DIGITS: usize = 10_000;

#[derive(Clone, Debug)]
pub struct Decimal {
    value: BigDecimal,
    negative_zero: bool,
}
impl PartialEq for Decimal {
    fn eq(&self, other: &Self) -> bool {
        self.value == other.value
    }
}
impl Eq for Decimal {}
impl PartialOrd for Decimal {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for Decimal {
    fn cmp(&self, other: &Self) -> Ordering {
        self.value.cmp(&other.value)
    }
}
impl FromStr for Decimal {
    type Err = String;
    fn from_str(input: &str) -> Result<Self> {
        let input = input.trim().replace('_', "");
        require(
            input.len() <= MAX_INPUT_DIGITS,
            "decimal input exceeds 10000 characters",
        )?;
        let value =
            BigDecimal::from_str(&input).map_err(|_| "expected finite decimal".to_string())?;
        let result = Self {
            negative_zero: input.starts_with('-') && value.is_zero(),
            value,
        };
        result.check_range()?;
        Ok(result)
    }
}
impl Decimal {
    pub fn integer(value: i64) -> Self {
        Self {
            value: BigDecimal::from(value),
            negative_zero: false,
        }
    }
    pub fn is_negative(&self) -> bool {
        self.value.is_negative() || self.negative_zero
    }
    pub fn is_zero(&self) -> bool {
        self.value.is_zero()
    }
    fn check_range(&self) -> Result<()> {
        let (coefficient, scale) = self.value.as_bigint_and_exponent();
        let digits = coefficient.abs().to_string().len() as i64;
        require(
            (-MAX_EXPONENT - 999..=MAX_EXPONENT + 999).contains(&scale),
            "decimal exponent overflow",
        )?;
        require(
            coefficient.is_zero() || digits - scale - 1 <= MAX_EXPONENT,
            "decimal overflow",
        )
    }
    pub fn constraints(
        &self,
        positive: bool,
        maximum: i64,
        max_digits: usize,
        decimal_places: usize,
    ) -> Result<()> {
        require(
            if positive {
                self > &Self::integer(0)
            } else {
                self >= &Self::integer(0)
            },
            "decimal value is below its minimum",
        )?;
        require(
            self <= &Self::integer(maximum),
            "decimal value exceeds its maximum",
        )?;
        let normalized = self.value.normalized();
        let (coefficient, scale) = normalized.as_bigint_and_exponent();
        let coefficient_digits = if coefficient.is_zero() {
            1
        } else {
            coefficient.abs().to_string().len()
        };
        let fractional = scale.max(0) as usize;
        let total = if scale < 0 {
            coefficient_digits + (-scale) as usize
        } else {
            coefficient_digits.max(fractional)
        };
        let whole = total - fractional;
        require(
            total <= max_digits
                && fractional <= decimal_places
                && whole <= max_digits - decimal_places,
            "decimal exceeds digit or decimal-place limits",
        )
    }
    fn rounded(value: BigDecimal, negative_zero: bool, precision: u32) -> Result<Self> {
        require(
            (1..=1000).contains(&precision),
            "decimal precision must be in 1..1000",
        )?;
        let mut value = value;
        // BigDecimal::with_precision_round also pads shorter values; Python's
        // context preserves their preferred exponent, so only reduce precision.
        if value.digits() > u64::from(precision) {
            value = value.with_precision_round(
                std::num::NonZeroU64::new(u64::from(precision)).unwrap(),
                RoundingMode::HalfEven,
            );
            if value.digits() > u64::from(precision) {
                value = value.with_precision_round(
                    std::num::NonZeroU64::new(u64::from(precision)).unwrap(),
                    RoundingMode::HalfEven,
                );
            }
        }
        // Match Decimal's default Emin/Emax and context-dependent Etiny.
        // Tiny results round to a signed zero at Etiny; zero's positive
        // exponent is clamped without inventing a numerical overflow.
        let (_, scale) = value.as_bigint_and_exponent();
        let tiny_scale = MAX_EXPONENT + i64::from(precision) - 1;
        if scale > tiny_scale {
            value = value.with_scale_round(tiny_scale, RoundingMode::HalfEven);
        }
        let (_, scale) = value.as_bigint_and_exponent();
        if value.is_zero() && scale < -MAX_EXPONENT {
            value = BigDecimal::new(BigInt::from(0), -MAX_EXPONENT);
        }
        let result = Self {
            negative_zero: negative_zero && value.is_zero(),
            value,
        };
        result.check_range()?;
        Ok(result)
    }
    fn sum(&self, other: &Self, subtract: bool, precision: u32) -> Result<Self> {
        self.check_range()?;
        other.check_range()?;
        let (left, left_scale) = self.value.as_bigint_and_exponent();
        let (right, right_scale) = other.value.as_bigint_and_exponent();
        let scale = left_scale.max(right_scale);
        let ten = BigInt::from(10);
        let left = left * ten.pow((scale - left_scale) as u32);
        let right = right * ten.pow((scale - right_scale) as u32);
        let coefficient = if subtract { left - right } else { left + right };
        let negative = coefficient.is_negative()
            || (coefficient.is_zero() && self.is_negative() && other.is_negative() != subtract);
        Self::rounded(BigDecimal::new(coefficient, scale), negative, precision)
    }
    pub fn add(&self, other: &Self, precision: u32) -> Result<Self> {
        self.sum(other, false, precision)
    }
    pub fn subtract(&self, other: &Self, precision: u32) -> Result<Self> {
        self.sum(other, true, precision)
    }
    pub fn divide(&self, other: &Self, precision: u32) -> Result<Self> {
        require(
            (1..=1000).contains(&precision),
            "decimal precision must be in 1..1000",
        )?;
        self.check_range()?;
        other.check_range()?;
        require(!other.is_zero(), "decimal division by zero")?;
        let (a, a_scale) = self.value.as_bigint_and_exponent();
        let (b, b_scale) = other.value.as_bigint_and_exponent();
        let preferred = a_scale
            .checked_sub(b_scale)
            .ok_or("decimal exponent overflow")?;
        let negative = self.is_negative() != other.is_negative();
        if a.is_zero() {
            return Self::rounded(BigDecimal::new(a, preferred), negative, precision);
        }
        let a = a.abs();
        let b = b.abs();
        let ten = BigInt::from(10);
        let difference = a.to_string().len() as i64 - b.to_string().len() as i64;
        let below = if difference >= 0 {
            a < &b * ten.pow(difference as u32)
        } else {
            &a * ten.pow((-difference) as u32) < b
        };
        let adjusted = difference - i64::from(below);
        require(adjusted - preferred <= MAX_EXPONENT, "decimal overflow")?;
        let scale = (preferred + i64::from(precision) - 1 - adjusted)
            .min(MAX_EXPONENT + i64::from(precision) - 1);
        let shift = scale - preferred;
        let (numerator, denominator) = if shift >= 0 {
            (a * ten.pow(shift as u32), b)
        } else {
            (a, b * ten.pow((-shift) as u32))
        };
        let mut coefficient = &numerator / &denominator;
        let remainder = &numerator % &denominator;
        let mut scale = scale;
        if remainder.is_zero() {
            while scale > preferred && (&coefficient % &ten).is_zero() {
                coefficient /= &ten;
                scale -= 1;
            }
        } else {
            let twice = remainder * 2;
            if twice > denominator || (twice == denominator && !(&coefficient % 2_u8).is_zero()) {
                coefficient += 1;
            }
        }
        if negative {
            coefficient = -coefficient;
        }
        Self::rounded(BigDecimal::new(coefficient, scale), negative, precision)
    }
    pub fn multiply(&self, other: &Self, precision: u32) -> Result<Self> {
        self.check_range()?;
        other.check_range()?;
        let (left, left_scale) = self.value.as_bigint_and_exponent();
        let (right, right_scale) = other.value.as_bigint_and_exponent();
        let scale = left_scale
            .checked_add(right_scale)
            .ok_or("decimal exponent overflow")?;
        Self::rounded(
            BigDecimal::new(left * right, scale),
            self.is_negative() != other.is_negative(),
            precision,
        )
    }
    /// Exact power-of-ten division with Decimal's preferred result exponent,
    /// then explicit context rounding. The supported source units need no other divisor.
    pub fn divide_power_ten(&self, power: u32, precision: u32) -> Result<Self> {
        self.check_range()?;
        let (mut coefficient, original_scale) = self.value.as_bigint_and_exponent();
        let mut scale = original_scale
            .checked_add(i64::from(power))
            .ok_or("decimal exponent overflow")?;
        if coefficient.is_zero() {
            scale = original_scale;
        } else {
            let ten = BigInt::from(10);
            while scale > original_scale && (&coefficient % &ten).is_zero() {
                coefficient /= &ten;
                scale -= 1;
            }
        }
        Self::rounded(
            BigDecimal::new(coefficient, scale),
            self.negative_zero,
            precision,
        )
    }
}
impl fmt::Display for Decimal {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let (coefficient, scale) = self.value.as_bigint_and_exponent();
        let digits = coefficient.abs().to_string();
        if self.is_negative() {
            f.write_str("-")?;
        }
        let adjusted = digits.len() as i64 - scale - 1;
        if scale >= 0 && adjusted >= -6 {
            if scale == 0 {
                return f.write_str(&digits);
            }
            if scale >= digits.len() as i64 {
                f.write_str("0.")?;
                for _ in 0..scale - digits.len() as i64 {
                    f.write_str("0")?;
                }
                f.write_str(&digits)
            } else {
                let split = digits.len() - scale as usize;
                write!(f, "{}.{}", &digits[..split], &digits[split..])
            }
        } else {
            f.write_str(&digits[..1])?;
            if digits.len() > 1 {
                write!(f, ".{}", &digits[1..])?;
            }
            write!(f, "E{adjusted:+}")
        }
    }
}
impl Serialize for Decimal {
    fn serialize<S: Serializer>(&self, s: S) -> std::result::Result<S::Ok, S::Error> {
        s.serialize_str(&self.to_string())
    }
}
impl<'de> Deserialize<'de> for Decimal {
    fn deserialize<D: Deserializer<'de>>(d: D) -> std::result::Result<Self, D::Error> {
        match Value::deserialize(d)? {
            Value::String(s) => s.parse().map_err(serde::de::Error::custom),
            // arbitrary_precision retains the JSON number text without an f64 step.
            Value::Number(n) => n.to_string().parse().map_err(serde::de::Error::custom),
            _ => Err(serde::de::Error::custom(
                "expected decimal string or number",
            )),
        }
    }
}
impl JsonSchema for Decimal {
    fn schema_name() -> Cow<'static, str> {
        "Decimal".into()
    }
    fn json_schema(_: &mut SchemaGenerator) -> Schema {
        json!({"type":"string","format":"decimal"})
            .try_into()
            .unwrap()
    }
    fn inline_schema() -> bool {
        true
    }
}
