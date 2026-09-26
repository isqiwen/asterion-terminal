//! Fixed, non-plugin mechanisms. No feature crate belongs in this dependency closure.
//!
//! The JSON entry point is a language-binding boundary, not a plugin dispatcher:
//! callers cannot register or replace any of these implementations.

pub mod archive;
pub mod artifacts;
pub mod authority;
pub mod communication;
pub mod database;
pub mod diagnostics;
pub mod directories;
pub mod environments;
pub mod events;
pub mod file_archives;
pub mod files;
pub mod lifetime;
pub mod plugins;
mod processes;
pub mod recovery;
pub mod secrets;
pub mod security;
pub mod storage;
pub mod supervisor;
pub mod tasks;
pub mod transport;

use serde::de::DeserializeOwned;
use serde_json::{Value, json};

fn decode<T: DeserializeOwned>(value: Value) -> Result<T, String> {
    serde_json::from_value(value).map_err(|error| format!("Invalid kernel input: {error}"))
}

pub fn invoke(operation: &str, input: Value) -> Result<Value, String> {
    match operation {
        "tasks.contract" => {
            let _: tasks::EmptyRequest = decode(input)?;
            Ok(json!({"states": tasks::State::ALL}))
        }
        "tasks.duration" => {
            let request: tasks::LeaseDurationRequest = decode(input)?;
            tasks::check_duration(request.lease_seconds)?;
            Ok(Value::Null)
        }
        "plugins.validate" => {
            let request: plugins::ValidationRequest = decode(input)?;
            serde_json::to_value(plugins::validate(&request.plugins)?)
                .map_err(|error| error.to_string())
        }
        "plugins.bindings" => {
            let request: plugins::BindingsRequest = decode(input)?;
            let plan = plugins::validate(&request.plugins)?;
            plugins::validate_grants(&request.plugins, &request.grants)?;
            Ok(json!({"order": plan.order}))
        }
        "database.config" => {
            #[derive(serde::Deserialize)]
            #[serde(deny_unknown_fields)]
            struct Request {
                url: String,
            }
            let request: Request = decode(input)?;
            database::config_from_url(&request.url)
        }
        "auth.worker_token" => {
            let request: authority::SecretRequest = decode(input)?;
            Ok(json!(authority::worker_token(&request.secret)))
        }
        "auth.forwarding_token" => {
            let request: authority::SecretRequest = decode(input)?;
            Ok(json!(authority::forwarding_token(&request.secret)))
        }
        "auth.forwarded" => {
            let request: authority::ForwardedRequest = decode(input)?;
            Ok(json!(authority::forwarded(
                &request.credential,
                &request.secret
            )))
        }
        "auth.subject" => {
            let request: authority::SubjectRequest = decode(input)?;
            Ok(json!(authority::subject(&request.session)))
        }
        "auth.grant_allows" => {
            let request: authority::GrantRequest = decode(input)?;
            Ok(json!(request.grant.allows(&request.method, &request.path)))
        }
        "auth.issue" => {
            let request: authority::IssueRequest = decode(input)?;
            serde_json::to_value(authority::issue(&request)?).map_err(|error| error.to_string())
        }
        "auth.authorize" => {
            let request: authority::AuthorizeRequest = decode(input)?;
            Ok(json!(authority::authorize(&request)?))
        }
        _ => Err(format!("Unknown kernel operation: {operation}")),
    }
}
