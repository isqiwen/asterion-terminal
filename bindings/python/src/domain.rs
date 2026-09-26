//! Immutable domain handle ownership; only argument/result conversion belongs here.
use asterion_instrument_catalog::{Catalog, ImportResolver, SourceResolver};
use asterion_trading_calendar::Calendar;
use pyo3::{exceptions::PyValueError, prelude::*};
use serde::{Serialize, de::DeserializeOwned};
use serde_json::Value;

enum Domain {
    Catalog(Catalog),
    Source(SourceResolver),
    Import(ImportResolver),
    Calendar(Calendar),
    Rules(Box<asterion_market_rules::Rules>),
    Roles(Box<asterion_role_registry::RoleRegistry>),
    RoleCalendar(Box<asterion_role_registry::OpeningCalendar>),
    ComputedRoles(Box<asterion_role_registry::ComputedRegistry>),
}

fn input<T: DeserializeOwned>(value: &str) -> Result<T, String> {
    serde_json::from_str(value).map_err(|e| e.to_string())
}
fn output(value: impl Serialize) -> Result<String, String> {
    serde_json::to_string(&value).map_err(|e| e.to_string())
}

#[pyclass(frozen, module = "asterion_bindings._native")]
pub struct DomainHandle {
    inner: Domain,
}

impl DomainHandle {
    fn dispatch(&self, operation: &str, value: &str) -> Result<String, String> {
        macro_rules! request {($($field:ident : $ty:ty),* $(,)?)=>{{
            #[derive(serde::Deserialize)]
            #[serde(deny_unknown_fields)]
            struct Request { $($field:$ty),* }
            input::<Request>(value)?
        }};}
        match (&self.inner, operation) {
            (Domain::Catalog(c), "id") => {
                request!();
                output(c.id())
            }
            (Domain::Catalog(c), "resolve") => {
                let r = request!(request:asterion_instrument_catalog::ResolutionRequest);
                output(c.resolve(&r.request)?)
            }
            (Domain::Source(c), "resolve") => {
                let r = request!(day:chrono::NaiveDate);
                output(c.resolve(r.day)?)
            }
            (Domain::Source(c), "validate_rows") => {
                let r = request!(rows:Vec<Value>,exchange:String);
                output(c.validate_rows(&r.rows, &r.exchange)?)
            }
            (Domain::Import(c), "resolve") => {
                let r = request!(contract:String,day:chrono::NaiveDate);
                output(c.resolve(&r.contract, r.day)?)
            }
            (Domain::Import(c), "validate_rows") => {
                let r = request!(rows:Vec<Value>);
                c.validate_rows(&r.rows)?;
                output(Value::Null)
            }
            (Domain::Calendar(c), "id") => {
                request!();
                output(c.id())
            }
            (Domain::Calendar(c), "spans") => {
                request!();
                output(c.spans())
            }
            (Domain::Calendar(c), "check_contract") => {
                let r = request!(contract:String);
                c.check_contract(&r.contract)?;
                output(Value::Null)
            }
            (Domain::Calendar(c), "daily") => {
                let r = request!(contract:String,day:chrono::NaiveDate);
                output(c.daily(&r.contract, r.day)?)
            }
            (Domain::Calendar(c), "resolve") => {
                let r = request!(contract:String,stamp:chrono::DateTime<chrono::FixedOffset>,boundary:asterion_trading_calendar::Boundary);
                output(c.resolve(&r.contract, r.stamp, r.boundary)?)
            }
            (Domain::Calendar(c), "validate_bar") => {
                let r = request!(contract:String,stamp:chrono::DateTime<chrono::FixedOffset>,day:chrono::NaiveDate,seconds:i64,boundary:asterion_trading_calendar::BarBoundary);
                output(c.validate_bar(&r.contract, r.stamp, r.day, r.seconds, r.boundary)?)
            }
            (Domain::Rules(rules), "id") => {
                request!();
                output(rules.id())
            }
            (Domain::Rules(rules), "at") => {
                let r = request!(day:chrono::NaiveDate);
                output(rules.at(r.day)?)
            }
            (Domain::Rules(rules), "cover") => {
                let r = request!(contract_id:String,start:chrono::NaiveDate,end:chrono::NaiveDate);
                rules.cover(&r.contract_id, r.start, r.end)?;
                output(Value::Null)
            }
            (Domain::Roles(registry), "id") => {
                request!();
                output(registry.id())
            }
            (Domain::Roles(registry), "resolve") => {
                let r = request!(query:asterion_role_registry::RoleQuery);
                output(registry.resolve(&r.query)?)
            }
            (Domain::RoleCalendar(calendar), "next_opening") => {
                let r = request!(observation_end:chrono::DateTime<chrono::FixedOffset>,available_at:chrono::DateTime<chrono::FixedOffset>);
                output(calendar.next_opening(r.observation_end, r.available_at)?)
            }
            (Domain::ComputedRoles(registry), "resolve") => {
                let r = request!(query:asterion_role_registry::RoleQuery);
                output(registry.resolve(&r.query)?)
            }
            _ => Err("unsupported operation for this domain handle".into()),
        }
    }
}

#[pymethods]
impl DomainHandle {
    #[new]
    fn new(py: Python<'_>, module: &str, model: &str, value: &str) -> PyResult<Self> {
        py.detach(|| {
            let value = input(value)?;
            let inner = match (module, model) {
                ("catalog", "ReferenceCatalog") => Domain::Catalog(Catalog::from_value(value)?),
                ("catalog", "SourceIdentity") => Domain::Source(SourceResolver::from_value(value)?),
                ("catalog", "ImportIdentity") => Domain::Import(ImportResolver::from_value(value)?),
                ("calendar", "TimeSpec") => Domain::Calendar(Calendar::from_value(value)?),
                ("rules", "RuleSpec") => {
                    Domain::Rules(Box::new(asterion_market_rules::Rules::from_value(value)?))
                }
                ("roles", "RoleSpec") => Domain::Roles(Box::new(
                    asterion_role_registry::RoleRegistry::from_value(value)?,
                )),
                ("roles", "RoleCalendar") => Domain::RoleCalendar(Box::new(
                    asterion_role_registry::OpeningCalendar::from_value(value)?,
                )),
                ("roles", "ComputedRoleIndex") => Domain::ComputedRoles(Box::new(
                    asterion_role_registry::ComputedRegistry::from_value(value)?,
                )),
                _ => return Err("unsupported immutable domain handle".to_string()),
            };
            Ok(Self { inner })
        })
        .map_err(PyValueError::new_err)
    }

    fn call(&self, py: Python<'_>, operation: &str, value: &str) -> PyResult<String> {
        py.detach(|| self.dispatch(operation, value))
            .map_err(PyValueError::new_err)
    }
}

pub fn register(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<DomainHandle>()
}
