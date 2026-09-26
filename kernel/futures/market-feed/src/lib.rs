//! Bounded quote state and freshness for one connection. Provider parsing,
//! watchlist persistence and display composition belong to applications.
use asterion_connections::Subscription;
use bigdecimal::BigDecimal;
use num_traits::ToPrimitive;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    collections::{BTreeMap, BTreeSet},
    str::FromStr,
};

pub type Result<T> = std::result::Result<T, String>;
const SUBSCRIPTIONS: usize = 50;

fn required_option<'de, D: serde::Deserializer<'de>, T: Deserialize<'de>>(
    deserializer: D,
) -> std::result::Result<Option<T>, D::Error> {
    Option::<T>::deserialize(deserializer)
}

#[derive(Clone, Debug, Deserialize, Serialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct QuoteEvent {
    pub exchange: String,
    pub symbol: String,
    #[serde(deserialize_with = "required_option")]
    pub last: Option<f64>,
    #[serde(deserialize_with = "required_option")]
    pub previous_settlement: Option<f64>,
    #[serde(deserialize_with = "required_option")]
    pub high: Option<f64>,
    #[serde(deserialize_with = "required_option")]
    pub low: Option<f64>,
    #[serde(deserialize_with = "required_option")]
    pub volume: Option<i64>,
    #[serde(deserialize_with = "required_option")]
    pub open_interest: Option<f64>,
    pub trading_day: String,
    pub action_day: String,
    pub source_time: String,
    #[serde(deserialize_with = "required_option")]
    pub event_at: Option<f64>,
    pub received_at: f64,
}
impl QuoteEvent {
    pub fn validate(&self) -> Result<()> {
        for value in [
            self.last,
            self.previous_settlement,
            self.high,
            self.low,
            self.open_interest,
            self.event_at,
            Some(self.received_at),
        ]
        .into_iter()
        .flatten()
        {
            finite(value)?;
        }
        for (value, maximum) in [
            (&self.exchange, 16),
            (&self.symbol, 32),
            (&self.trading_day, 16),
            (&self.action_day, 16),
            (&self.source_time, 32),
        ] {
            if value.len() > maximum {
                return Err("Quote source field exceeds its bound".into());
            }
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, JsonSchema, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum QuoteStatus {
    Current,
    Disconnected,
    TimeUnknown,
    TimeAhead,
    NotUpdated,
    Delayed,
}

#[derive(Clone, Debug, Deserialize, Serialize, JsonSchema)]
pub struct Quote {
    #[serde(flatten)]
    pub observation: QuoteEvent,
    #[serde(deserialize_with = "required_option")]
    pub change: Option<f64>,
    #[serde(deserialize_with = "required_option")]
    pub change_percent: Option<f64>,
    pub status: QuoteStatus,
    pub stale: bool,
}
impl Quote {
    fn validate(&self) -> Result<()> {
        self.observation.validate()?;
        for value in [self.change, self.change_percent].into_iter().flatten() {
            finite(value)?;
        }
        if self.stale != (self.status != QuoteStatus::Current) {
            return Err("Inconsistent quote freshness".into());
        }
        Ok(())
    }
}
fn finite(value: f64) -> Result<()> {
    if value.is_finite() {
        Ok(())
    } else {
        Err("Quote number must be finite".into())
    }
}
fn decimal(value: f64) -> Result<BigDecimal> {
    BigDecimal::from_str(&value.to_string()).map_err(|_| "Invalid quote price".into())
}
fn status(event: &QuoteEvent, ready: bool, now: f64) -> QuoteStatus {
    if !ready {
        QuoteStatus::Disconnected
    } else if event.event_at.is_none() {
        QuoteStatus::TimeUnknown
    } else if event.event_at.is_some_and(|stamp| stamp > now + 5.) {
        QuoteStatus::TimeAhead
    } else if now - event.received_at > 30. {
        QuoteStatus::NotUpdated
    } else if event.event_at.is_some_and(|stamp| now - stamp > 30.) {
        QuoteStatus::Delayed
    } else {
        QuoteStatus::Current
    }
}

#[derive(Clone)]
struct RecordedQuote {
    generation: u64,
    fresh: bool,
    quote: Quote,
}

#[derive(Default)]
pub struct QuoteBook {
    revision: Option<u64>,
    subscriptions: Vec<Subscription>,
    quotes: Vec<RecordedQuote>,
    errors: BTreeMap<String, (u64, String)>,
}
impl QuoteBook {
    pub fn bind(&mut self, revision: u64) -> Result<()> {
        if revision == 0 {
            return Err("Connection revision must be positive".into());
        }
        if self.revision != Some(revision) {
            self.quotes.clear();
            self.errors.clear();
            self.revision = Some(revision);
        }
        Ok(())
    }
    pub fn subscriptions(&mut self, subscriptions: Vec<Subscription>) -> Result<()> {
        let mut symbols = BTreeSet::new();
        if subscriptions.len() > SUBSCRIPTIONS {
            return Err("Too many quote subscriptions".into());
        }
        for item in &subscriptions {
            item.validate().map_err(|error| error.to_string())?;
            if !symbols.insert(&item.symbol) {
                return Err("Duplicate quote subscription".into());
            }
        }
        self.quotes.retain(|record| {
            let quote = &record.quote;
            subscriptions.iter().any(|item| {
                item.exchange == quote.observation.exchange
                    && item.symbol == quote.observation.symbol
            })
        });
        self.subscriptions = subscriptions;
        self.errors.clear();
        Ok(())
    }
    pub fn error(
        &mut self,
        generation: u64,
        current: u64,
        symbol: &str,
        detail: &str,
    ) -> Result<()> {
        if detail.len() > 4096 {
            return Err("Subscription error exceeds its bound".into());
        }
        if generation == current && self.subscriptions.iter().any(|item| item.symbol == symbol) {
            self.errors
                .insert(symbol.into(), (generation, detail.into()));
        }
        Ok(())
    }
    pub fn connection_event(&mut self, generation: u64, current: u64, kind: &str) {
        if generation != current {
            return;
        }
        if matches!(kind, "connecting" | "reconnecting" | "error") {
            for record in &mut self.quotes {
                record.fresh = false;
            }
        }
        if matches!(kind, "connecting" | "reconnecting" | "connected") {
            self.errors.clear();
        }
    }
    pub fn ingest(
        &mut self,
        generation: u64,
        current: u64,
        ready: bool,
        mut event: QuoteEvent,
        now: f64,
    ) -> Result<()> {
        finite(now)?;
        event.validate()?;
        if generation != current || !ready {
            return Ok(());
        }
        let Some(subscription) = self
            .subscriptions
            .iter()
            .find(|item| item.symbol == event.symbol)
        else {
            return Ok(());
        };
        if !event.exchange.is_empty() && event.exchange != subscription.exchange {
            self.errors
                .insert(event.symbol, (generation, "来源交易所与自选不一致".into()));
            return Ok(());
        }
        event.exchange = subscription.exchange.clone();
        let previous = self
            .quotes
            .iter()
            .position(|record| record.quote.observation.symbol == event.symbol);
        if previous.is_some_and(|index| {
            self.quotes[index].generation == generation
                && self.quotes[index].fresh
                && self.quotes[index]
                    .quote
                    .observation
                    .event_at
                    .is_some_and(|stamp| {
                        stamp <= now + 5. && event.event_at.is_some_and(|new| new < stamp)
                    })
        }) {
            return Ok(());
        }
        let change = match (event.last, event.previous_settlement) {
            (Some(last), Some(base)) if base > 0. => Some(decimal(last)? - decimal(base)?),
            _ => None,
        };
        let percent = change
            .as_ref()
            .map(|change| {
                Ok::<_, String>(
                    (change / decimal(event.previous_settlement.expect("change has a base"))?)
                        * BigDecimal::from(100),
                )
            })
            .transpose()?;
        let convert = |number: &BigDecimal| {
            number
                .to_f64()
                .filter(|v| v.is_finite())
                .ok_or_else(|| "Quote change is out of range".to_string())
        };
        let quote = Quote {
            observation: event,
            change: change.as_ref().map(convert).transpose()?,
            change_percent: percent.as_ref().map(convert).transpose()?,
            status: QuoteStatus::Current,
            stale: false,
        };
        if let Some(index) = previous {
            self.quotes[index] = RecordedQuote {
                generation,
                fresh: true,
                quote,
            };
        } else {
            self.quotes.push(RecordedQuote {
                generation,
                fresh: true,
                quote,
            });
        }
        Ok(())
    }
    pub fn snapshot(&self, current: u64, ready: bool, now: f64) -> Result<Value> {
        finite(now)?;
        let quotes: Vec<_> = self
            .quotes
            .iter()
            .cloned()
            .map(|record| {
                let mut quote = record.quote;
                quote.status = if ready && (record.generation != current || !record.fresh) {
                    QuoteStatus::NotUpdated
                } else {
                    status(&quote.observation, ready, now)
                };
                quote.stale = quote.status != QuoteStatus::Current;
                quote
            })
            .collect();
        let errors: BTreeMap<_, _> = self
            .errors
            .iter()
            .filter(|(_, (generation, _))| *generation == current)
            .map(|(symbol, (_, detail))| (symbol, detail))
            .collect();
        Ok(json!({"quotes":quotes,"subscription_errors":errors}))
    }
}

pub fn invoke(operation: &str, value: Value) -> Result<Value> {
    match operation {
        "schema" => {
            if value != json!({}) {
                return Err("Schema request must be an empty object".into());
            }
            #[derive(JsonSchema)]
            #[allow(dead_code)]
            struct Models {
                event: QuoteEvent,
                quote: Quote,
            }
            let mut schema =
                serde_json::to_value(schemars::schema_for!(Models)).map_err(|e| e.to_string())?;
            for model in ["QuoteEvent", "Quote"] {
                schema["$defs"][model]["required"] = json!(
                    schema["$defs"][model]["properties"]
                        .as_object()
                        .expect("model fields")
                        .keys()
                        .collect::<Vec<_>>()
                );
            }
            Ok(schema)
        }
        "validate" => {
            #[derive(Deserialize)]
            #[serde(deny_unknown_fields)]
            struct Request {
                model: String,
                value: Value,
            }
            let request: Request = serde_json::from_value(value).map_err(|e| e.to_string())?;
            match request.model.as_str() {
                "QuoteEvent" => serde_json::from_value::<QuoteEvent>(request.value)
                    .map_err(|e| e.to_string())?
                    .validate()?,
                "Quote" => serde_json::from_value::<Quote>(request.value)
                    .map_err(|e| e.to_string())?
                    .validate()?,
                _ => return Err("Unknown market feed model".into()),
            }
            Ok(Value::Null)
        }
        _ => Err("Unknown market feed operation".into()),
    }
}
