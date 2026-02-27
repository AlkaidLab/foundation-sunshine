# Building
Sunshine binaries are built using [CMake](https://cmake.org) and requires `cmake` > 3.25.

## Building Locally

### Dependencies

#### Linux
Dependencies vary depending on the distribution. You can reference our
[linux_build.sh](https://github.com/LizardByte/Sunshine/blob/master/scripts/linux_build.sh) script for a list of
dependencies we use in Debian-based and Fedora-based distributions. Please submit a PR if you would like to extend the
script to support other distributions.

##### CUDA Toolkit
Sunshine requires CUDA Toolkit for NVFBC capture. There are two caveats to CUDA:

1. The version installed depends on the version of GCC.
2. The version of CUDA you use will determine compatibility with various GPU generations.
   At the time of writing, the recommended version to use is CUDA ~11.8.
   See [CUDA compatibility](https://docs.nvidia.com/deploy/cuda-compatibility/index.html) for more info.

@tip{To install older versions, select the appropriate run file based on your desired CUDA version and architecture
according to [CUDA Toolkit Archive](https://developer.nvidia.com/cuda-toolkit-archive)}

#### macOS
You can either use [Homebrew](https://brew.sh) or [MacPorts](https://www.macports.org) to install dependencies.

##### Homebrew
```bash
dependencies=(
  "boost"  # Optional
  "cmake"
  "doxygen"  # Optional, for docs
  "graphviz"  # Optional, for docs
  "icu4c"  # Optional, if boost is not installed
  "miniupnpc"
  "node"
  "openssl@3"
  "opus"
  "pkg-config"
)
brew install ${dependencies[@]}
```

If there are issues with an SSL header that is not found:

@tabs{
  @tab{ Intel | ```bash
    ln -s /usr/local/opt/openssl/include/openssl /usr/local/include/openssl
    ```}
  @tab{ Apple Silicon | ```bash
    ln -s /opt/homebrew/opt/openssl/include/openssl /opt/homebrew/include/openssl
    ```
  }
}

##### MacPorts
```bash
dependencies=(
  "cmake"
  "curl"
  "doxygen"  # Optional, for docs
  "graphviz"  # Optional, for docs
  "libopus"
  "miniupnpc"
  "npm9"
  "pkgconfig"
)
sudo port install ${dependencies[@]}
```

#### Windows
First you need to install [MSYS2](https://www.msys2.org), then startup "MSYS2 UCRT64" and execute the following
commands.

##### Update all packages
```bash
pacman -Syu
```

##### Install dependencies
```bash
dependencies=(
  "doxygen"  # Optional, for docs
  "git"
  "mingw-w64-ucrt-x86_64-boost"  # Optional
  "mingw-w64-ucrt-x86_64-cmake"
  "mingw-w64-ucrt-x86_64-cppwinrt"
  "mingw-w64-ucrt-x86_64-curl-winssl"
  "mingw-w64-ucrt-x86_64-graphviz"  # Optional, for docs
  "mingw-w64-ucrt-x86_64-MinHook"
  "mingw-w64-ucrt-x86_64-miniupnpc"
  "mingw-w64-ucrt-x86_64-nlohmann-json"
  "mingw-w64-ucrt-x86_64-nodejs"
  "mingw-w64-ucrt-x86_64-nsis"
  "mingw-w64-ucrt-x86_64-onevpl"
  "mingw-w64-ucrt-x86_64-openssl"
  "mingw-w64-ucrt-x86_64-opus"
  "mingw-w64-ucrt-x86_64-toolchain"
)
pacman -S ${dependencies[@]}
```

### Clone
Ensure [git](https://git-scm.com) is installed on your system, then clone the repository using the following command:

```bash
git clone https://github.com/lizardbyte/sunshine.git --recurse-submodules
cd sunshine
mkdir build
```

### Build

```bash
cmake -B build -G Ninja -S .
ninja -C build
```

@tip{Available build options can be found in
[options.cmake](https://github.com/LizardByte/Sunshine/blob/master/cmake/prep/options.cmake).}

#### macOS-Specific Build Notes

##### FFmpeg Header Priority (Critical)

Sunshine uses bundled FFmpeg static libraries located in `third-party/build-deps/dist/Darwin-arm64/`. The CMake configuration ensures these bundled headers take priority over system headers (e.g., Homebrew FFmpeg).

**Why this matters:** System FFmpeg headers may have different struct layouts than the bundled libraries, causing crashes. The build system automatically handles this, but if you modify CMake files, ensure:

```cmake
include_directories(BEFORE SYSTEM ${FFMPEG_INCLUDE_DIRS})
```

comes before any system include directories.

##### Clean Build Recommended

For macOS builds, especially after pulling updates, perform a clean build:

```bash
rm -rf build
cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

##### Common Build Issues

**Issue:** `ld: library not found for -lssl`
**Solution:** Install OpenSSL via Homebrew and ensure it's linked:
```bash
brew install openssl@3
ln -s /opt/homebrew/opt/openssl/include/openssl /opt/homebrew/include/openssl
```

**Issue:** `fatal error: 'boost/...hpp' file not found`
**Solution:** Install Boost via Homebrew:
```bash
brew install boost
```

**Issue:** Build succeeds but binary crashes on launch
**Solution:** This may indicate FFmpeg ABI mismatch. Perform a clean rebuild and check `docs/troubleshooting.md` for details.

##### Testing on macOS

After building, run the test suite to verify core functionality:

```bash
# Build with tests enabled
cmake -B build -G Ninja -S . -DBUILD_TESTS=ON
ninja -C build

# Run all tests
./build/tests/test_sunshine

# Run only macOS-specific tests
./build/tests/test_sunshine --gtest_filter="ColorspaceTest.*:MacOSEncoderTest.*"
```

Expected results:
- 22+ tests should pass (ColorspaceTest + MacOSEncoderTest)
- VideoToolbox encoder should initialize successfully
- No SIGSEGV or memory corruption errors

##### Packaging for Distribution

To create a DMG installer:

```bash
# Ensure Release build
cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release
ninja -C build

# Generate DMG
cpack -G DragNDrop --config ./build/CPackConfig.cmake
```

Output: `Sunshine-<version>-Darwin-arm64.dmg`

**Note:** The generated DMG is not code-signed or notarized. Users will need to right-click and select "Open" to bypass Gatekeeper on first launch.

### Package

@tabs{
  @tab{Linux | @tabs{
    @tab{deb | ```bash
      cpack -G DEB --config ./build/CPackConfig.cmake
      ```}
    @tab{rpm | ```bash
      cpack -G RPM --config ./build/CPackConfig.cmake
      ```}
  }}
  @tab{macOS | @tabs{
    @tab{DragNDrop | ```bash
      cpack -G DragNDrop --config ./build/CPackConfig.cmake
      ```}
  }}
  @tab{Windows | @tabs{
    @tab{Installer | ```bash
      cpack -G NSIS --config ./build/CPackConfig.cmake
      ```}
    @tab{Portable | ```bash
      cpack -G ZIP --config ./build/CPackConfig.cmake
      ```}
  }}
}

### Remote Build
It may be beneficial to build remotely in some cases. This will enable easier building on different operating systems.

1. Fork the project
2. Activate workflows
3. Trigger the *CI* workflow manually
4. Download the artifacts/binaries from the workflow run summary

<div class="section_buttons">

| Previous                              |                            Next |
|:--------------------------------------|--------------------------------:|
| [Troubleshooting](troubleshooting.md) | [Contributing](contributing.md) |

</div>
