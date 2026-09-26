//! Communication ingress of native operations. Every request receives a causal
//! context and deadline from the fixed kernel rules; the response carries it
//! back. Forwarded operations get the same treatment from the Python process.
use crate::error;
use asterion_kernel::communication::{self, Error};
use axum::{
    extract::Request,
    http::{HeaderName, HeaderValue, StatusCode},
    middleware::Next,
    response::Response,
};
use serde_json::Value;

pub(crate) const HEADER: HeaderName = HeaderName::from_static("x-asterion-context");
/// Budget of a request that arrives without a parent context.
const TIMEOUT_SECONDS: f64 = 60.0;

/// The request's communication context, as a kernel `Context` value.
#[derive(Clone)]
pub(crate) struct Trace(pub Value);

pub(crate) async fn ingress(mut request: Request, next: Next) -> Response {
    let raw = request.headers().get(&HEADER).map(HeaderValue::as_bytes);
    let trace = match communication::ingress(raw, TIMEOUT_SECONDS) {
        Ok(trace) => trace,
        Err(Error::Deadline) => {
            return error(
                StatusCode::REQUEST_TIMEOUT,
                "请求已超过截止时间",
                "DEADLINE_EXCEEDED",
            );
        }
        Err(_) => {
            return error(
                StatusCode::UNPROCESSABLE_ENTITY,
                "通信上下文不符合当前契约",
                "INVALID_COMMUNICATION",
            );
        }
    };
    let value = serde_json::to_value(&trace).expect("serializable context");
    let header = HeaderValue::from_str(&value.to_string()).expect("ASCII context");
    request.extensions_mut().insert(Trace(value));
    let mut response = next.run(request).await;
    response.headers_mut().insert(HEADER, header);
    response
}
