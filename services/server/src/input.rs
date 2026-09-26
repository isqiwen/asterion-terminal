//! Request body validation of native operations: JSON of the declared shape,
//! unknown fields ignored, constraint violations refused as `422 INVALID_INPUT`.
use crate::Failure;
use axum::body::Bytes;
use regex::Regex;
use serde::de::DeserializeOwned;

pub(crate) struct Invalid;

pub(crate) fn length(value: &str, min: usize, max: usize) -> Result<(), Invalid> {
    (min..=max)
        .contains(&value.chars().count())
        .then_some(())
        .ok_or(Invalid)
}

pub(crate) fn pattern(value: &str, pattern: &Regex) -> Result<(), Invalid> {
    pattern.is_match(value).then_some(()).ok_or(Invalid)
}

pub(crate) fn body<T: DeserializeOwned>(
    content: &Bytes,
    check: impl Fn(&T) -> Result<(), Invalid>,
) -> Result<T, Failure> {
    let value: T = serde_json::from_slice(content).map_err(|_| Failure::invalid())?;
    check(&value).map_err(|_| Failure::invalid())?;
    Ok(value)
}
