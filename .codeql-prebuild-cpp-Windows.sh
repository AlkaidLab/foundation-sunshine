# install dependencies for C++ analysis
set -e

# update pacman
pacman --noconfirm -Syu

gcc_version="15.1.0-5"

broken_deps=(
  "mingw-w64-ucrt-x86_64-gcc"
  "mingw-w64-ucrt-x86_64-gcc-libs"
)

tarballs=""
for dep in "${broken_deps[@]}"; do
  tarball="${dep}-${gcc_version}-any.pkg.tar.zst"

  # download and install working version
  wget https://repo.msys2.org/mingw/ucrt64/${tarball}

  tarballs="${tarballs} ${tarball}"
done

# install broken dependencies
if [ -n "$tarballs" ]; then
  pacman -U --noconfirm ${tarballs}
fi

# install dependencies
dependencies=(
  "git"
  "mingw-w64-ucrt-x86_64-cmake"
  "mingw-w64-ucrt-x86_64-cppwinrt"
  "mingw-w64-ucrt-x86_64-curl-winssl"
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

# Note: mingw-w64-ucrt-x86_64-rust conflicts with fixed gcc-15.1.0-5
# We install Rust via rustup instead

pacman -Syu --noconfirm --ignore="$(IFS=,; echo "${broken_deps[*]}")" "${dependencies[@]}"

# install Rust via rustup (for Tauri GUI)
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📦 Installing Rust via rustup (required for Tauri GUI)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# check if cargo already exists
if command -v cargo &> /dev/null; then
  echo "✅ Rust already installed: $(cargo --version)"
else
  echo "📥 Downloading and installing rustup..."
  
  # download rustup-init for Windows
  curl --proto '=https' --tlsv1.2 -sSf https://win.rustup.rs/x86_64 -o /tmp/rustup-init.exe
  
  # install rustup with defaults (non-interactive)
  /tmp/rustup-init.exe -y --default-toolchain stable --profile minimal
  
  # add cargo to PATH for current session
  export PATH="$HOME/.cargo/bin:$PATH"
  
  # verify installation
  if command -v cargo &> /dev/null; then
    echo "✅ Rust installed successfully: $(cargo --version)"
    echo "   rustc: $(rustc --version)"
  else
    echo "❌ Rust installation failed!"
    echo "   Please install manually from: https://rustup.rs/"
    exit 1
  fi
fi

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# build
mkdir -p build
cmake \
  -B build \
  -G Ninja \
  -S . \
  -DBUILD_DOCS=OFF \
  -DBUILD_WERROR=ON
ninja -C build

# skip autobuild
echo "skip_autobuild=true" >> "$GITHUB_OUTPUT"