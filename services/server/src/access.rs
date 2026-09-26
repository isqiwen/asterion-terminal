//! Account policy of native operations and functional scope credentials.
use crate::identity::Session;
use crate::input::{body, length};
use crate::{Authorized, Entry, Failure, session_token};
use asterion_kernel::authority::{self, IssueRequest};
use axum::{
    body::Bytes,
    extract::{FromRequestParts, State},
    http::{StatusCode, request::Parts},
    response::{IntoResponse, Response},
};
use serde::Deserialize;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

/// A request of an unlocked account, or any request when the runtime does not
/// require accounts (development). Refusals match the internal process's.
pub struct Unlocked;

impl FromRequestParts<Arc<Entry>> for Unlocked {
    type Rejection = Response;
    async fn from_request_parts(parts: &mut Parts, entry: &Arc<Entry>) -> Result<Self, Response> {
        if !entry.require_account {
            return Ok(Self);
        }
        let token = session_token(&parts.headers);
        let identity = entry.identity.clone();
        let session = tokio::task::spawn_blocking(move || identity.session(&token))
            .await
            .unwrap_or(Session::Unavailable);
        let (status, detail, code) = match session {
            Session::Unlocked(_) => return Ok(Self),
            Session::Locked(_) => (
                StatusCode::LOCKED,
                "终端已锁定，请输入 PIN",
                "TERMINAL_LOCKED",
            ),
            Session::Expired => (
                StatusCode::UNAUTHORIZED,
                "登录已过期，请重新登录",
                "SESSION_EXPIRED",
            ),
            Session::Unsupported => (
                StatusCode::CONFLICT,
                "账号安全状态不受支持，缺少注册时设置的 PIN",
                "UNSUPPORTED_ACCOUNT_SECURITY",
            ),
            Session::Unavailable => (
                StatusCode::SERVICE_UNAVAILABLE,
                "账户服务暂不可用，请稍后重试",
                "ACCOUNT_UNAVAILABLE",
            ),
        };
        Err(Failure::new(status, detail, code).into_response())
    }
}

#[derive(Deserialize)]
struct ScopeRequest {
    scope: String,
}

/// A five-minute credential for one product scope, bound to the caller's
/// account session. Only the trusted workbench (root) may request one.
async fn issue(
    State(entry): State<Arc<Entry>>,
    Authorized(principal): Authorized,
    _: Unlocked,
    parts: axum::http::HeaderMap,
    content: Bytes,
) -> Result<Response, Failure> {
    let request = body::<ScopeRequest>(&content, |b| length(&b.scope, 1, 80))?;
    if principal != "root" {
        return Err(Failure::new(
            StatusCode::FORBIDDEN,
            "只有可信工作台可申请功能授权",
            "FORBIDDEN",
        ));
    }
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| Failure::internal())?
        .as_secs_f64();
    let issued = authority::issue(&IssueRequest {
        secret: entry.secret.clone(),
        policies: entry.authorization.policies.clone(),
        scope: request.scope,
        session: session_token(&parts),
        now,
    })
    .map_err(|message| Failure::new(StatusCode::UNPROCESSABLE_ENTITY, &message, "INVALID_INPUT"))?;
    Ok(
        axum::Json(serde_json::json!({"token": issued.token, "expires": issued.expires}))
            .into_response(),
    )
}

pub(crate) fn operations() -> Vec<crate::Operation> {
    vec![crate::Operation::post("/api/v1/access/scopes", issue)]
}
