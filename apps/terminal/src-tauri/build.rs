use std::{env, path::PathBuf};
fn main() {
    let build = PathBuf::from(
        env::var("ASTERION_CPP_BUILD")
            .expect("Use pnpm desktop or pnpm desktop:build to prepare C++"),
    );
    println!("cargo:rerun-if-env-changed=ASTERION_CPP_BUILD");
    println!("cargo:rustc-link-search=native={}", build.display());
    let suffix = if env::var("CARGO_CFG_TARGET_OS").unwrap() == "windows" {
        ".lib"
    } else {
        ".a"
    };
    let prefix = if suffix == ".lib" { "" } else { "lib" };
    for library in [
        "asterion_terminal_api",
        "asterion_node_firewall",
        "asterion_csv",
        "asterion_sma",
        "asterion_protocol",
        "asterion_domain",
        "asterion_kernel",
        "asterion_foundation",
    ] {
        let archive = build.join(format!("{prefix}{library}{suffix}"));
        assert!(
            archive.exists(),
            "Missing C++ archive: {}",
            archive.display()
        );
        println!("cargo:rerun-if-changed={}", archive.display());
        println!("cargo:rustc-link-lib=static={library}");
    }
    let manifest = build.join("generators/native-link.txt");
    println!("cargo:rerun-if-changed={}", manifest.display());
    for line in std::fs::read_to_string(&manifest)
        .expect("Conan native link manifest missing")
        .lines()
    {
        let (kind, value) = line.split_once('=').expect("Invalid native link manifest");
        match kind {
            "search" => println!("cargo:rustc-link-search=native={value}"),
            "static" => println!("cargo:rustc-link-lib=static={value}"),
            "system" => println!("cargo:rustc-link-lib={value}"),
            "framework" => println!("cargo:rustc-link-lib=framework={value}"),
            _ => panic!("Unknown native link entry"),
        }
    }
    match env::var("CARGO_CFG_TARGET_OS").unwrap().as_str() {
        "macos" => println!("cargo:rustc-link-lib=c++"),
        "linux" => println!("cargo:rustc-link-lib=stdc++"),
        "windows" => println!("cargo:rustc-link-lib=advapi32"),
        _ => {}
    }
    tauri_build::build();
}
