//! SMTP delivery of verification codes, configured in `identity-mail.json`
//! under the data root. The configuration file is written by the operator; the
//! service only reads it.
use crate::identity::{Delivery, IdentityError};
use lettre::{
    Message, SmtpTransport, Transport,
    message::header::ContentType,
    transport::smtp::{authentication::Credentials, client::Tls, client::TlsParameters},
};
use serde::Deserialize;
use std::path::PathBuf;
use std::time::Duration;

#[derive(Deserialize)]
struct Config {
    sender: String,
    host: String,
    port: u16,
    security: String,
    #[serde(default)]
    username: String,
    #[serde(default)]
    password: String,
}

pub struct Mailer {
    path: PathBuf,
}

fn failed() -> IdentityError {
    IdentityError {
        status: 502,
        code: "MAIL_FAILED",
        message: "邮件发送失败，请检查服务器配置后重试".into(),
    }
}

impl Mailer {
    pub fn new(root: PathBuf) -> Self {
        Self {
            path: root.join("identity-mail.json"),
        }
    }

    fn config(&self) -> Result<Option<Config>, IdentityError> {
        match std::fs::read(&self.path) {
            Ok(content) => serde_json::from_slice(&content)
                .map(Some)
                .map_err(|_| failed()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(_) => Err(failed()),
        }
    }
}

impl Delivery for Mailer {
    fn send(&self, email: &str, code: &str, purpose: &str) -> Result<(), IdentityError> {
        let Some(config) = self.config()? else {
            return Err(IdentityError {
                status: 503,
                code: "MAIL_NOT_CONFIGURED",
                message: "请先在设置 → 邮件服务中配置发信服务器".into(),
            });
        };
        let subject = if purpose == "verify" {
            "邮箱验证码"
        } else {
            "重置密码验证码"
        };
        let message = Message::builder()
            .from(config.sender.parse().map_err(|_| failed())?)
            .to(email.parse().map_err(|_| failed())?)
            .subject(format!("Asterion Terminal · {subject}"))
            .header(ContentType::TEXT_PLAIN)
            .body(format!(
                "你的 Asterion Terminal 验证码是：{code}\n\n10 分钟内有效，只能使用一次。若非本人操作，请忽略此邮件。\n"
            ))
            .map_err(|_| failed())?;
        let tls = || TlsParameters::new(config.host.clone()).map_err(|_| failed());
        let mut builder = SmtpTransport::builder_dangerous(&config.host)
            .port(config.port)
            .timeout(Some(Duration::from_secs(10)));
        builder = match config.security.as_str() {
            "tls" => builder.tls(Tls::Wrapper(tls()?)),
            "starttls" => builder.tls(Tls::Required(tls()?)),
            _ => builder.tls(Tls::None),
        };
        if !config.username.is_empty() {
            builder = builder.credentials(Credentials::new(config.username, config.password));
        }
        builder.build().send(&message).map_err(|error| {
            // Permanent refusals of the recipient address, not of the login.
            match error.status().map(u16::from) {
                Some(550 | 551 | 553) => IdentityError {
                    status: 502,
                    code: "MAIL_FAILED",
                    message: "邮件服务器拒绝了收件地址，请检查邮箱".into(),
                },
                _ => failed(),
            }
        })?;
        Ok(())
    }
}
