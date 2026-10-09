// Embeds the Windows manifest (modern controls, sharp text on high-DPI screens, long paths) in pure Rust, and the
// icon (the same ../res/qoqdir.ico as the C++ version) through windres, which comes with the C++ toolchain
// (C:\mingw64\bin): without it on the PATH the build still succeeds, with a warning and no icon.
use embed_manifest::manifest::{DpiAwareness, Setting};
use embed_manifest::{embed_manifest, new_manifest};

fn main() {
    if std::env::var_os("CARGO_CFG_WINDOWS").is_some() {
        embed_manifest(
            new_manifest("Qoqdir")
                .dpi_awareness(DpiAwareness::PerMonitorV2)
                .long_path_aware(Setting::Enabled),
        )
        .expect("unable to embed manifest");
        let icon = format!("{}/icon.o", std::env::var("OUT_DIR").unwrap());
        let made = std::process::Command::new("windres").args(["icon.rc", "-I", "../res", "-O", "coff", "-o", &icon]).status();
        if made.is_ok_and(|s| s.success()) {
            println!("cargo:rustc-link-arg={icon}");
        } else {
            println!("cargo:warning=windres not found: the exe gets no icon");
        }
    }
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=icon.rc");
    println!("cargo:rerun-if-changed=../res/qoqdir.ico");
}
