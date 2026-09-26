//! The same Rust types define the native checks and generated language DTOs.
use crate::*;
use schemars::JsonSchema;
use serde::Deserialize;
use serde_json::{Value, json};

pub fn invoke(operation: &str, value: Value) -> std::result::Result<Value, String> {
    execute(operation, value).map_err(|error| error.to_string())
}
fn execute(operation: &str, value: Value) -> Result<Value> {
    bounded_value(&value, MAX_STATE_BYTES)?;
    match operation {
        "schema" => {
            if value != json!({}) {
                return Err(Error::invalid("Schema request must be an empty object"));
            }
            #[derive(JsonSchema)]
            #[allow(dead_code)]
            struct Models {
                subscription: Subscription,
                field: ConfigField,
                connector: ConnectorDescriptor,
                profile: ConnectionProfile,
                channel: Channel,
                channel_state: ChannelState,
                read_request: ReadRequest,
                read_batch: ReadBatch,
            }
            Ok(serde_json::to_value(schemars::schema_for!(Models))?)
        }
        "validate" => {
            #[derive(Deserialize)]
            #[serde(deny_unknown_fields)]
            struct Request {
                model: String,
                value: Value,
            }
            let request: Request = serde_json::from_value(value)?;
            macro_rules! model {
                ($type:ty) => {{
                    let model: $type = serde_json::from_value(request.value)?;
                    model.validate()?;
                    Ok(serde_json::to_value(model)?)
                }};
            }
            match request.model.as_str() {
                "Subscription" => model!(Subscription),
                "ConfigField" => model!(ConfigField),
                "ConnectorDescriptor" => model!(ConnectorDescriptor),
                "ConnectionProfile" => model!(ConnectionProfile),
                "ChannelState" => model!(ChannelState),
                "ReadRequest" => model!(ReadRequest),
                "ReadBatch" => model!(ReadBatch),
                _ => Err(Error::invalid("Unknown connections model")),
            }
        }
        _ => Err(Error::invalid("Unknown connections operation")),
    }
}
