//! Python's text forms of values, which stored data and identifiers use:
//! `str(Decimal)`, `repr(float)` and `date.fromisoformat`.
use chrono::{Datelike, NaiveDate};

/// A decimal's sign, coefficient digits and exponent, as Python's `Decimal`.
pub(crate) struct Parts {
    pub(crate) negative: bool,
    pub(crate) digits: String,
    pub(crate) exponent: i64,
}

pub(crate) fn parts(text: &str) -> Option<Parts> {
    let text = text.trim().replace('_', "");
    let (mantissa, exponent) = match text.find(['e', 'E']) {
        Some(at) => (&text[..at], text[at + 1..].parse::<i64>().ok()?),
        None => (text.as_str(), 0),
    };
    let negative = mantissa.starts_with('-');
    let unsigned = mantissa.strip_prefix(['+', '-']).unwrap_or(mantissa);
    let (whole, fraction) = unsigned.split_once('.').unwrap_or((unsigned, ""));
    if (whole.is_empty() && fraction.is_empty())
        || !whole
            .bytes()
            .chain(fraction.bytes())
            .all(|b| b.is_ascii_digit())
    {
        return None;
    }
    let digits = format!("{whole}{fraction}")
        .trim_start_matches('0')
        .to_string();
    Some(Parts {
        negative,
        digits: if digits.is_empty() {
            "0".into()
        } else {
            digits
        },
        exponent: exponent - fraction.len() as i64,
    })
}

pub(crate) fn render(parts: &Parts) -> String {
    let Parts {
        negative,
        digits,
        exponent,
    } = parts;
    let adjusted = exponent + digits.len() as i64 - 1;
    let body = if *exponent <= 0 && adjusted >= -6 {
        if *exponent == 0 {
            digits.clone()
        } else {
            let point = digits.len() as i64 + exponent;
            if point > 0 {
                format!(
                    "{}.{}",
                    &digits[..point as usize],
                    &digits[point as usize..]
                )
            } else {
                format!("0.{}{digits}", "0".repeat((-point) as usize))
            }
        }
    } else {
        let tail = if digits.len() > 1 {
            format!(".{}", &digits[1..])
        } else {
            String::new()
        };
        format!(
            "{}{tail}E{}{adjusted}",
            &digits[..1],
            if adjusted >= 0 { "+" } else { "" }
        )
    };
    if *negative { format!("-{body}") } else { body }
}

/// `str(Decimal(text))`: the canonical text the tables store decimals as.
pub fn decimal_text(text: &str) -> Option<String> {
    parts(text).map(|parts| render(&parts))
}

/// `int(Decimal(text))`: the integral part, truncated toward zero.
pub fn integral(text: &str) -> Option<i64> {
    let parts = parts(text)?;
    let digits = if parts.exponent >= 0 {
        format!("{}{}", parts.digits, "0".repeat(parts.exponent as usize))
    } else {
        let keep = parts.digits.len() as i64 + parts.exponent;
        if keep <= 0 {
            "0".into()
        } else {
            parts.digits[..keep as usize].to_string()
        }
    };
    let value: i64 = digits.parse().ok()?;
    Some(if parts.negative { -value } else { value })
}

/// Python's `repr(float)`: shortest round-trip digits.
pub fn float_repr(value: f64) -> String {
    if value.is_nan() {
        return "NaN".into();
    }
    if value.is_infinite() {
        return if value > 0.0 { "Infinity" } else { "-Infinity" }.into();
    }
    let text = format!("{value:?}");
    // Rust debug output is round-trip shortest; adjust exponent spelling.
    if let Some((mantissa, exponent)) = text.split_once('e') {
        let exponent: i32 = exponent.parse().unwrap_or(0);
        let mantissa = mantissa.strip_suffix(".0").unwrap_or(mantissa);
        return format!(
            "{mantissa}e{}{:02}",
            if exponent < 0 { '-' } else { '+' },
            exponent.abs()
        );
    }
    let absolute = value.abs();
    if absolute != 0.0 && !(1e-4..1e16).contains(&absolute) {
        return scientific(value);
    }
    text
}

fn scientific(value: f64) -> String {
    let text = format!("{value:e}");
    let (mantissa, exponent) = text.split_once('e').expect("exponent");
    let exponent: i32 = exponent.parse().unwrap_or(0);
    format!(
        "{mantissa}e{}{:02}",
        if exponent < 0 { '-' } else { '+' },
        exponent.abs()
    )
}

/// Python's `date.fromisoformat`: calendar, basic and week dates.
pub fn iso_date(text: &str) -> Result<NaiveDate, String> {
    let invalid = || format!("Invalid isoformat string: '{text}'");
    let parsed = if text.len() == 10 && text.as_bytes()[4] == b'-' && text.as_bytes()[7] == b'-' {
        NaiveDate::parse_from_str(text, "%Y-%m-%d").ok()
    } else if text.len() == 8 && text.bytes().all(|b| b.is_ascii_digit()) {
        NaiveDate::parse_from_str(text, "%Y%m%d").ok()
    } else if text.len() == 10 && text.as_bytes()[4] == b'-' && text.as_bytes()[5] == b'W' {
        NaiveDate::parse_from_str(&text.replace('-', ""), "%GW%V%u").ok()
    } else if text.len() == 8 && text.as_bytes()[4] == b'W' {
        NaiveDate::parse_from_str(text, "%GW%V%u").ok()
    } else {
        None
    };
    // Python dates start at year 1.
    parsed.filter(|day| day.year() >= 1).ok_or_else(invalid)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn float_repr_matches_python() {
        for (number, text) in [
            (1.0, "1.0"),
            (1e16, "1e+16"),
            (1.5e-5, "1.5e-05"),
            (123456.789, "123456.789"),
            (0.0, "0.0"),
            (0.1, "0.1"),
            (-2.5, "-2.5"),
        ] {
            assert_eq!(float_repr(number), text, "{number}");
        }
    }
}
