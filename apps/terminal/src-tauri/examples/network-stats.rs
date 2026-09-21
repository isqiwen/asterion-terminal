//! Inspect the same system counters used by the setup page.
#[path = "../src/network_stats.rs"]
mod network_stats;
fn main() {
    for _ in 0..3 {
        println!(
            "{}",
            serde_json::to_string(&network_stats::snapshot()).unwrap()
        );
        std::thread::sleep(std::time::Duration::from_secs(1));
    }
}
