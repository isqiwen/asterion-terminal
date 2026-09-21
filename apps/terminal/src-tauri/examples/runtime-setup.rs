//! Release verification entry: uses the exact same installer as the desktop host.
#[path = "../src/network_stats.rs"]
mod network_stats;
#[path = "../src/runtime_setup.rs"]
mod runtime_setup;
fn main() {
    let args: Vec<_> = std::env::args_os().skip(1).collect();
    if args.len() != 2 {
        eprintln!("Usage: runtime-setup SETUP_DIRECTORY STATE_DIRECTORY");
        std::process::exit(2);
    }
    let result = runtime_setup::Setup::load(
        std::path::Path::new(&args[0]),
        std::path::Path::new(&args[1]),
    )
    .and_then(|s| {
        s.install(|p| eprintln!("{}", serde_json::to_string(&p).unwrap()))?;
        println!("{}", s.root.display());
        Ok(())
    });
    if let Err(error) = result {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
