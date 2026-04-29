# Rust Tray Library

This directory contains a Rust implementation of the system tray library, designed to replace the original C `tray` library.

## Features

- Cross-platform support (Windows, Linux, macOS)
- Compatible C API matching the original `tray.h` header
- Uses the `tray-icon` Rust crate for the underlying implementation
- Better maintainability and reduced cross-platform adaptation code

## Prerequisites

- [Rust](https://rustup.rs/) (latest stable version recommended)
- Cargo (comes with Rust)

## Building

The library is automatically built by CMake when `SUNSHINE_USE_RUST_TRAY=ON` is set.

### Manual Build

```bash
cd rust_tray
cargo build --release
```

The static library will be generated at:
- Windows (MSVC): `target/release/tray.lib`
- Windows (MinGW): `target/release/libtray.a`
- Linux/macOS: `target/release/libtray.a`

## CMake Integration

To enable the Rust tray library, configure CMake with:

```bash
cmake -DSUNSHINE_USE_RUST_TRAY=ON ..
```

## API

The library provides the following C-compatible functions:

- `tray_init(struct tray *tray)` - Initialize the tray icon
- `tray_update(struct tray *tray)` - Update the tray icon and menu
- `tray_loop(int blocking)` - Run one iteration of the UI loop
- `tray_exit(void)` - Terminate the UI loop

See `../third-party/tray/src/tray.h` for the complete API definition.

## Architecture

```
rust_tray/
├── Cargo.toml      # Rust package manifest
├── build.rs        # Build script (platform-specific setup)
├── src/
│   ├── lib.rs      # Main library implementation
│   └── ffi.rs      # FFI type definitions
└── README.md       # This file
```

## Notes

- The Rust implementation maintains full API compatibility with the original C library
- No changes are required to `src/system_tray.cpp`
- The original `third-party/tray/src/tray.h` header is still used for the C++ code
