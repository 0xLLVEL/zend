use std::env;
use std::fs;
use std::path::PathBuf;

fn main() {
    let builds = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../builds");
    let active_file = builds.join("ACTIVE");
    let mut build = String::new();
    let mut client = String::new();

    println!("cargo:rerun-if-changed={}", active_file.display());
    if let Ok(name) = fs::read_to_string(&active_file) {
        let name = name.trim();
        let manifest = builds.join(name).join("build.json");
        println!("cargo:rerun-if-changed={}", manifest.display());
        if let Ok(text) = fs::read_to_string(&manifest) {
            if let Ok(json) = serde_json::from_str::<serde_json::Value>(&text) {
                build = json
                    .get("build")
                    .and_then(|value| value.as_str())
                    .unwrap_or_default()
                    .to_owned();
                client = json
                    .get("client")
                    .and_then(|value| value.as_str())
                    .unwrap_or_default()
                    .to_owned();
            }
        }
    }

    println!("cargo:rustc-env=ZEND_CLIENT_BUILD={build}");
    println!("cargo:rustc-env=ZEND_CLIENT_VERSION={client}");
    tauri_build::build()
}
