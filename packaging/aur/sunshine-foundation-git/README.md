# sunshine-foundation-git (AUR)

Arch packaging for **Foundation Sunshine** — the [AlkaidLab/foundation-sunshine][upstream]
fork of [LizardByte/Sunshine][lizardbyte] (here mirrored at [QiE2035/foundation-sunshine][fork]),
tracking the `linux-migration` branch where the Windows feature set (virtual display,
clipboard sync, mic redirect, HDR10+ tone mapping, ABR, tray) is ported to Linux.

Package name: `sunshine-foundation-git` — `provides=('sunshine')`,
`conflicts=('sunshine' 'sunshine-git' 'sunshine-bin')`.

## Files

| file | purpose |
| --- | --- |
| `PKGBUILD` | builds the branch from source; every submodule the build needs is a pinned `source` entry |
| `sunshine-foundation-git.install` | sets the file capabilities and reloads the udev rules after (un)install |
| `.SRCINFO` | AUR metadata, generated from `PKGBUILD` |

## Build / install locally

```bash
cd packaging/aur/sunshine-foundation-git
makepkg -si          # default flags: no CUDA, VAAPI + software encoding
```

The build is long (Sunshine links a lot of static archives) and needs npm to build
the web UI. `prepare()` sparse-checks out the two oversized submodules — the
Windows-only `third-party/AMF` SDK (~1.2 GB → headers only) and
`third-party/build-deps` (~600 MB → this machine's `dist/Linux-<arch>` slice,
~66 MB) — so budget a few GB of free space for `$srcdir` and the build tree.

Then:

```bash
systemctl --user enable --now sunshine     # or just run: sunshine
```

Web UI: <https://localhost:47990> — configuration lives in `~/.config/sunshine`.
Only one instance may run at a time (ports 47984/47989/47990/48010).

### NVIDIA / NVENC

CUDA is **off** by default — the `cuda` package is an AUR package and is not a
base dependency. To build with NVENC support, edit the top of `PKGBUILD`:

```bash
_use_cuda=true
```

and add `'cuda'` to `makedepends` (the `cuda` optdepend then becomes a real
dependency). Then `makepkg -si` as usual. VAAPI (AMD/Intel) and software
encoding are always enabled.

## Things that bite (already handled here)

* **Boost is pinned to an exact version.** `cmake/dependencies/Boost_Sunshine.cmake`
  does `find_package(Boost CONFIG ${BOOST_VERSION} EXACT ...)`. If Arch's boost is
  not exactly `$_boost_version`, CMake silently downloads and builds Boost from
  GitHub during configure — a network fetch inside `build()`. Bump
  `_boost_version` (and the `boost=`/`boost-libs=` depends entries) whenever Arch
  bumps boost.
* **Submodules cannot be fetched by makepkg.** makepkg does not check out gitlinks
  and the AUR build chroot has no network, so all 28 submodules (including nested
  ones under `third-party/{doxyconfig,moonlight-common-c,tray}`) are declared as
  `source=()` entries pinned by `#commit=` and placed into the tree by
  `prepare()`. `third-party/build-deps` supplies the pre-built static
  FFmpeg/x264/x265/SVT-AV1/hdr10plus archives under `dist/Linux-<arch>` — no
  system ffmpeg is used.
* **`options=(!strip)`.** makepkg's strip would remove the file capabilities the
  install script sets; Sunshine cannot grab KMS or drive the virtual display
  without them.
* **The installed binary is versioned** (`/usr/bin/sunshine-<version>`) with
  `/usr/bin/sunshine` as a symlink, so the install script resolves it with
  `readlink -f` before calling `setcap`.

## Keeping the package in sync with the branch

1. `pkgver()` derives the version from the checked-out tree, so a `makepkg -f`
   refresh after a branch update needs no manual `pkgver` edit.
2. If the branch added, removed or re-pinned a submodule, update `source=()` and
   `_submodule_paths`. Get the current gitlinks with:

   ```bash
   git -C /path/to/foundation-sunshine ls-tree -r HEAD | awk '$2=="commit"{print $3, $4}'
   ```

3. Regenerate `PKGBUILD`-derived metadata (the AUR rejects a stale `.SRCINFO`):

   ```bash
   cd packaging/aur/sunshine-foundation-git
   makepkg --printsrcinfo > .SRCINFO
   ```

4. `pkgrel=1` on a new upstream revision; bump `pkgrel` only for packaging-only
   changes.

## Publishing to the AUR

```bash
git clone ssh://aur@aur.archlinux.org/sunshine-foundation-git.git
cp PKGBUILD .SRCINFO sunshine-foundation-git.install aur-repo/
cd aur-repo && git add -A && git commit -m "Update to ..." && git push
```

The AUR requires the package name to be unclaimed and the `Maintainer:` line in
`PKGBUILD` to carry a real contact address.

[upstream]: https://github.com/AlkaidLab/foundation-sunshine
[fork]: https://github.com/QiE2035/foundation-sunshine
[lizardbyte]: https://github.com/LizardByte/Sunshine
