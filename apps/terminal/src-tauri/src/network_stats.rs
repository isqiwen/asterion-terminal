//! Read host interface counters, never attribute unrelated traffic to this installer.
use serde::Serialize;
use std::{fs, path::Path, sync::OnceLock, time::Instant};
#[derive(Clone, Serialize)]
pub struct NetworkSnapshot {
    pub received: u64,
    pub sent: u64,
    pub interfaces: String,
    pub sampled_ms: u64,
}
pub fn snapshot() -> Option<NetworkSnapshot> {
    read_interfaces(Path::new("/sys/class/net"))
}
fn read_interfaces(root: &Path) -> Option<NetworkSnapshot> {
    static START: OnceLock<Instant> = OnceLock::new();
    let start = START.get_or_init(Instant::now);
    let mut interfaces = Vec::new();
    let (mut received, mut sent) = (0u64, 0u64);
    // Physical devices avoid counting loopback, bridges, veth and VPN traffic twice.
    for entry in fs::read_dir(root).ok()? {
        let entry = entry.ok()?;
        let path = entry.path();
        if !path.join("device").exists() {
            continue;
        }
        let rx = fs::read_to_string(path.join("statistics/rx_bytes"))
            .ok()?
            .trim()
            .parse::<u64>()
            .ok()?;
        let tx = fs::read_to_string(path.join("statistics/tx_bytes"))
            .ok()?
            .trim()
            .parse::<u64>()
            .ok()?;
        received = received.checked_add(rx)?;
        sent = sent.checked_add(tx)?;
        interfaces.push(entry.file_name().to_string_lossy().into_owned());
    }
    if interfaces.is_empty() {
        return None;
    }
    interfaces.sort();
    Some(NetworkSnapshot {
        received,
        sent,
        interfaces: interfaces.join(","),
        sampled_ms: start.elapsed().as_millis() as u64,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn excludes_virtual_interfaces_and_reports_unavailable_counters() {
        let root = std::env::temp_dir().join(format!("asterion-network-{}", std::process::id()));
        fs::create_dir_all(root.join("eth0/device")).unwrap();
        for (name, rx, tx) in [("eth0", 120, 30), ("lo", 999, 999)] {
            let dir = root.join(name).join("statistics");
            fs::create_dir_all(&dir).unwrap();
            fs::write(dir.join("rx_bytes"), rx.to_string()).unwrap();
            fs::write(dir.join("tx_bytes"), tx.to_string()).unwrap();
        }
        let sample = read_interfaces(&root).unwrap();
        assert_eq!((sample.received, sample.sent), (120, 30));
        assert_eq!(sample.interfaces, "eth0");
        fs::write(root.join("eth0/statistics/rx_bytes"), "unavailable").unwrap();
        assert!(read_interfaces(&root).is_none());
        fs::remove_dir_all(root).unwrap();
    }
}
