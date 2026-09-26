//! `asterion-server --listen 127.0.0.1:PORT --upstream http://127.0.0.1:PORT`
//! with `ASTERION_TOKEN`, `ASTERION_DATABASE_URL` and `ASTERION_DATA_ROOT` in
//! the environment, and optionally `ASTERION_ACCOUNT_VERIFICATION` (`local` or
//! `email`, default `local`), `ASTERION_REQUIRE_ACCOUNT` (`true` or `false`,
//! default `false`) and `ASTERION_LEASE_SECONDS` (default 60).
//!
//! The runtime secret comes from the environment, never from arguments that
//! other local processes can read.
use asterion_server::{
    AUTHORIZATION, Authorization, FORWARDED_ROUTES, Identity, Mailer, RouteTable, Settings, Store,
    Verification, application,
};
use std::net::SocketAddr;
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;
use tokio::signal::unix::{SignalKind, signal};

fn arguments() -> Result<(SocketAddr, String), String> {
    let mut listen = None;
    let mut upstream = None;
    let mut values = std::env::args().skip(1);
    while let Some(name) = values.next() {
        let value = values
            .next()
            .ok_or_else(|| format!("Missing value for {name}"))?;
        match name.as_str() {
            "--listen" if listen.is_none() => listen = Some(value),
            "--upstream" if upstream.is_none() => upstream = Some(value),
            _ => return Err(format!("Unsupported argument: {name}")),
        }
    }
    let listen: SocketAddr = listen
        .ok_or("--listen is required")?
        .parse()
        .map_err(|_| "--listen must be an IP:port address")?;
    if !listen.ip().is_loopback() {
        return Err("--listen must be a loopback address".into());
    }
    Ok((listen, upstream.ok_or("--upstream is required")?))
}

async fn stopped() {
    let mut terminate = signal(SignalKind::terminate()).expect("SIGTERM handler");
    let mut interrupt = signal(SignalKind::interrupt()).expect("SIGINT handler");
    tokio::select! {
        _ = terminate.recv() => {}
        _ = interrupt.recv() => {}
    }
}

#[tokio::main]
async fn main() -> ExitCode {
    let result = async {
        let (listen, upstream) = arguments()?;
        let routes = RouteTable::parse(FORWARDED_ROUTES)?;
        let authorization = Authorization::parse(AUTHORIZATION)?;
        let variable = |name: &str| std::env::var(name).map_err(|_| format!("{name} is required"));
        let secret = variable("ASTERION_TOKEN")?;
        let database = variable("ASTERION_DATABASE_URL")?;
        let root = PathBuf::from(variable("ASTERION_DATA_ROOT")?);
        let optional =
            |name: &str, default: &str| std::env::var(name).unwrap_or_else(|_| default.to_string());
        let verification =
            Verification::parse(&optional("ASTERION_ACCOUNT_VERIFICATION", "local"))?;
        let require_account = match optional("ASTERION_REQUIRE_ACCOUNT", "false").as_str() {
            "true" => true,
            "false" => false,
            _ => return Err("ASTERION_REQUIRE_ACCOUNT must be true or false".into()),
        };
        let lease_seconds: u32 = optional("ASTERION_LEASE_SECONDS", "60")
            .parse()
            .map_err(|_| "ASTERION_LEASE_SECONDS must be a whole number of seconds")?;
        let key = secret.clone();
        let data_root = root.clone();
        let (store, identity) = tokio::task::spawn_blocking(move || {
            let store = Arc::new(Store::open(&database).map_err(|e| e.to_string())?);
            let identity = Identity::new(
                store.clone(),
                &key,
                verification,
                Box::new(Mailer::new(root.clone())),
            )?;
            Ok::<_, String>((store, identity))
        })
        .await
        .map_err(|e| e.to_string())??;
        let app = application(
            Settings {
                upstream,
                secret,
                require_account,
                lease_seconds: f64::from(lease_seconds),
                data_root,
            },
            routes,
            authorization,
            store,
            Arc::new(identity),
        )?;
        let listener = tokio::net::TcpListener::bind(listen)
            .await
            .map_err(|e| format!("Cannot listen on {listen}: {e}"))?;
        axum::serve(listener, app)
            .with_graceful_shutdown(stopped())
            .await
            .map_err(|e| e.to_string())
    }
    .await;
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("asterion-server: {message}");
            ExitCode::FAILURE
        }
    }
}
