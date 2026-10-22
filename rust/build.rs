// The exe's resources are the C++ version's own (../res/qoqdir.rc): the manifest (modern controls, sharp text on
// high-DPI screens, long paths), the icon and the version block. windres, which comes with the C++ toolchain, builds
// them. Without it the build stops: an exe without its manifest would look and scale wrong.
fn main() {
    if std::env::var_os("CARGO_CFG_WINDOWS").is_some() {
        let res = format!("{}/qoqdir_res.o", std::env::var("OUT_DIR").unwrap());
        match std::process::Command::new("windres").args(["../res/qoqdir.rc", "-O", "coff", "-o", &res]).status() {
            Ok(done) if done.success() => println!("cargo:rustc-link-arg={res}"),
            Ok(_) => panic!("windres could not build ../res/qoqdir.rc"),
            Err(_) => panic!("windres not found: it comes with MinGW-w64 (docs/BUILDING.md, section 1)"),
        }
    }
    for file in ["build.rs", "../res/qoqdir.rc", "../res/qoqdir.manifest", "../res/qoqdir.ico", "../src/version.h"] {
        println!("cargo:rerun-if-changed={file}");
    }
}
