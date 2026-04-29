//! Build script for sunshine_tray
//!
//! Note: We manually define the C structures in lib.rs instead of using bindgen
//! to avoid requiring LLVM/Clang installation on all build machines.

fn main() {
    // Tell cargo to rerun this script if the header file changes
    println!("cargo:rerun-if-changed=../third-party/tray/src/tray.h");
    println!("cargo:rerun-if-changed=build.rs");

    // Note: We manually define the FFI structures in src/ffi.rs
    // to match the original tray.h header file.

    // Platform-specific linker flags
    #[cfg(target_os = "windows")]
    {
        println!("cargo:rustc-link-lib=user32");
        println!("cargo:rustc-link-lib=gdi32");
        println!("cargo:rustc-link-lib=shell32");
        println!("cargo:rustc-link-lib=ole32");
        println!("cargo:rustc-link-lib=comctl32");
    }
}
