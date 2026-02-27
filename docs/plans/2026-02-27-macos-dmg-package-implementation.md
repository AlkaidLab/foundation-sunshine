# macOS DMG Package and Documentation Cleanup - Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Create a production-ready DMG installer for macOS and consolidate temporary debugging documentation into official docs.

**Architecture:** Extract key information from 8 temporary MD files into official documentation (docs/troubleshooting.md, docs/building.md, CLAUDE.md), perform clean Release build, generate DMG package with CPack, and cleanup temporary files.

**Tech Stack:** CMake, Ninja, CPack (DragNDrop generator), Markdown

---

## Task 1: Update docs/troubleshooting.md with macOS-Specific Issues

**Files:**
- Modify: `docs/troubleshooting.md`
- Reference: `ROOT_CAUSE_FIX.md`, `ENCODER_FIX_SUMMARY.md`, `MACOS_MOUSE_FIX_SUMMARY.md`

**Step 1: Read existing troubleshooting.md structure**

Run: `head -50 docs/troubleshooting.md`
Expected: See existing sections and formatting style

**Step 2: Read temporary MD files for content extraction**

Run: `cat ROOT_CAUSE_FIX.md ENCODER_FIX_SUMMARY.md MACOS_MOUSE_FIX_SUMMARY.md`
Expected: Full content of all three files

**Step 3: Add macOS-Specific Issues section to troubleshooting.md**

Add new section at appropriate location (after platform-specific sections if they exist, or near the end):

```markdown
## macOS-Specific Issues

### FFmpeg ABI Mismatch (SIGSEGV in av_hwframe_ctx_init)

**Symptom:**
- Server crashes with SIGSEGV during encoder initialization
- Crash occurs in `av_hwframe_ctx_init` function
- Debug shows `AVHWDeviceContext->type` has incorrect value

**Root Cause:**
Sunshine compiles with system Homebrew FFmpeg headers (`/opt/homebrew/include`) but links against bundled FFmpeg static libraries (`third-party/build-deps/dist/Darwin-arm64/lib`). The structure layouts differ between FFmpeg versions, causing memory corruption.

**Solution:**
Ensure bundled FFmpeg headers have priority in CMake configuration. This is already fixed in `cmake/compile_definitions/common.cmake`:

```cmake
# FFmpeg bundled headers must come BEFORE system headers
include_directories(BEFORE SYSTEM ${FFMPEG_INCLUDE_DIRS})
```

If you encounter this issue, verify your CMake configuration and rebuild from scratch:
```bash
rm -rf build
cmake -B build -G Ninja -S .
ninja -C build
```

---

### Encoder Initialization Failure (Invalid pixel format -1)

**Symptom:**
- Error: `Invalid video pixel format: -1`
- Error: `Picture size 2000x0 is invalid`
- All encoders (VideoToolbox and software) fail to initialize

**Root Cause:**
The `sunshine_colorspace_t` struct in `src/video_colorspace.h` had no default values, leading to uninitialized members when the struct was created.

**Solution:**
This is already fixed with default values in the struct definition:

```cpp
struct sunshine_colorspace_t {
  colorspace_e colorspace = colorspace_e::rec709;
  bool full_range = false;
  unsigned bit_depth = 8;
};
```

If you're building from an older commit, update to the latest code or manually add these defaults.

---

### Mouse Scroll Wheel Not Working

**Symptom:**
- Mouse movement works but scroll wheel has no effect
- Scrolling in remote applications doesn't respond

**Root Cause:**
macOS input code used `kCGScrollEventUnitLine` which has insufficient precision on modern macOS versions.

**Solution:**
This is already fixed in `src/platform/macos/input.cpp` to use `kCGScrollEventUnitPixel`:

```cpp
CGEventRef event = CGEventCreateScrollWheelEvent(
    macos_input->source,
    kCGScrollEventUnitPixel,  // Changed from kCGScrollEventUnitLine
    2,
    scrollY,
    scrollX
);
```

---

### Mouse Input Delay on Stream Start

**Symptom:**
- Mouse doesn't respond for ~250ms when first entering stream
- Initial clicks/movements are ignored

**Root Cause:**
macOS has a default 250ms event suppression interval to prevent event loops.

**Solution:**
This is already fixed in `src/platform/macos/input.cpp`:

```cpp
macos_input->source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
CGEventSourceSetLocalEventsSuppressionInterval(macos_input->source, 0.0);
```

---

### VideoToolbox Encoder Best Practices

**Tips for optimal VideoToolbox performance:**

1. **Use NV12 pixel format** for 8-bit content (most common)
2. **Use P010 pixel format** for 10-bit HDR content
3. **Ensure proper dimension alignment**: Width and height should be even numbers for YUV420
4. **Check hardware support**: Not all Macs support all encoder features
5. **Monitor encoder logs**: Look for "Encoder [videotoolbox] passed" in startup logs

**Common VideoToolbox errors:**

- `kVTParameterErr`: Usually indicates invalid encoder parameters
- `kVTAllocationFailedErr`: System resources exhausted, try lowering resolution/bitrate
- `kVTPixelTransferNotSupportedErr`: Pixel format conversion not supported by hardware
```

**Step 4: Verify the section was added correctly**

Run: `grep -A 5 "macOS-Specific Issues" docs/troubleshooting.md`
Expected: See the new section header and first few lines

**Step 5: Commit the documentation update**

```bash
git add docs/troubleshooting.md
git commit -m "docs: add macOS-specific troubleshooting section

- FFmpeg ABI mismatch solution
- Encoder initialization fixes
- Mouse input issues and solutions
- VideoToolbox best practices

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 2: Update docs/building.md with macOS Build Notes

**Files:**
- Modify: `docs/building.md`
- Reference: `ROOT_CAUSE_FIX.md`, `STAGE1_TEST_REPORT.md`

**Step 1: Read existing building.md to find macOS section**

Run: `grep -n "macOS\|Darwin" docs/building.md | head -20`
Expected: Line numbers where macOS is mentioned

**Step 2: Locate the macOS build instructions section**

Run: `sed -n '/<line_from_step1>/,/<line_from_step1>+50/p' docs/building.md`
Expected: Current macOS build instructions

**Step 3: Add macOS-specific build notes**

Add a new subsection under macOS build instructions (or create one if it doesn't exist):

```markdown
### macOS-Specific Build Notes

#### FFmpeg Header Priority (Critical)

Sunshine uses bundled FFmpeg static libraries located in `third-party/build-deps/dist/Darwin-arm64/`. The CMake configuration ensures these bundled headers take priority over system headers (e.g., Homebrew FFmpeg).

**Why this matters:** System FFmpeg headers may have different struct layouts than the bundled libraries, causing crashes. The build system automatically handles this, but if you modify CMake files, ensure:

```cmake
include_directories(BEFORE SYSTEM ${FFMPEG_INCLUDE_DIRS})
```

comes before any system include directories.

#### Clean Build Recommended

For macOS builds, especially after pulling updates, perform a clean build:

```bash
rm -rf build
cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

#### Common Build Issues

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

#### Testing on macOS

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

#### Packaging for Distribution

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
```

**Step 4: Verify the additions**

Run: `grep -A 3 "macOS-Specific Build Notes" docs/building.md`
Expected: See the new section

**Step 5: Commit the changes**

```bash
git add docs/building.md
git commit -m "docs: add macOS-specific build notes and troubleshooting

- FFmpeg header priority explanation
- Clean build recommendations
- Common build issues and solutions
- Testing procedures
- DMG packaging instructions

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 3: Update CLAUDE.md with macOS Build Warnings

**Files:**
- Modify: `CLAUDE.md`

**Step 1: Read current CLAUDE.md structure**

Run: `grep -n "## " CLAUDE.md | head -20`
Expected: Section headers with line numbers

**Step 2: Find the appropriate section to add macOS notes**

Look for "Platform Specific" or "macOS" section, or add after "Build Commands"

**Step 3: Add macOS build warnings section**

Add after the "Platform Specific Notes" section or create a new one:

```markdown
### macOS Build Warnings

**Critical:** When building on macOS, always perform a clean build after:
- Pulling updates from git
- Modifying CMake configuration files
- Switching branches
- Installing/updating Homebrew packages (especially FFmpeg)

**Why:** macOS builds use bundled FFmpeg static libraries. System headers (Homebrew) can interfere if CMake cache is stale, causing runtime crashes.

**Clean build command:**
```bash
rm -rf build
cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

**Testing:** After building, verify encoders work:
```bash
./build/sunshine 2>&1 | grep -i "encoder.*passed"
```

Expected output: `Info: Found H.264 encoder: h264_videotoolbox [videotoolbox]`

**Troubleshooting:** If you encounter crashes or encoder failures, see `docs/troubleshooting.md` → "macOS-Specific Issues"
```

**Step 4: Verify the addition**

Run: `grep -A 5 "macOS Build Warnings" CLAUDE.md`
Expected: See the new section

**Step 5: Commit the changes**

```bash
git add CLAUDE.md
git commit -m "docs: add macOS build warnings to CLAUDE.md

- Clean build requirements
- Testing verification steps
- Reference to troubleshooting docs

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 4: Clean Build

**Files:**
- Remove: `build/` directory
- Create: Fresh build artifacts

**Step 1: Remove existing build directory**

Run: `rm -rf build`
Expected: No output, build directory deleted

**Step 2: Verify build directory is gone**

Run: `ls -la | grep build`
Expected: No output (or only .gitignore entries)

**Step 3: Reconfigure CMake with Release mode**

Run: `cmake -B build -G Ninja -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON`
Expected:
```
-- The C compiler identification is AppleClang
-- The CXX compiler identification is AppleClang
...
-- Configuring done
-- Generating done
-- Build files have been written to: /Volumes/ISCSI-Disk/Folder/foundation-sunshine/build
```

**Step 4: Build with Ninja**

Run: `ninja -C build`
Expected:
```
[1/XXX] Building CXX object ...
...
[XXX/XXX] Linking CXX executable sunshine
```

Build should complete with 0 errors (warnings are acceptable)

**Step 5: Verify binary was created**

Run: `file build/sunshine`
Expected: `build/sunshine: Mach-O 64-bit executable arm64`

**Step 6: Run tests to validate build**

Run: `./build/tests/test_sunshine --gtest_filter="ColorspaceTest.*:MacOSEncoderTest.*"`
Expected:
```
[==========] Running 22 tests from 2 test suites.
...
[  PASSED  ] 22 tests.
```

**Step 7: Commit build configuration (if any files changed)**

```bash
git status
# If any CMake files were modified during configuration, commit them
git add -u
git commit -m "build: clean Release build for DMG packaging

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 5: Generate DMG Package

**Files:**
- Create: `Sunshine-<version>-Darwin-arm64.dmg`

**Step 1: Verify CPack configuration exists**

Run: `ls -la build/CPackConfig.cmake`
Expected: File exists with recent timestamp

**Step 2: Check current version string**

Run: `grep "CPACK_PACKAGE_VERSION" build/CPackConfig.cmake`
Expected: Version string like `2026.0224.230256`

**Step 3: Generate DMG package**

Run: `cpack -G DragNDrop --config ./build/CPackConfig.cmake`
Expected:
```
CPack: Create package using DragNDrop
CPack: Install projects
CPack: - Run preinstall target for: Sunshine
CPack: - Install project: Sunshine []
CPack: Create package
CPack: - package: /Volumes/ISCSI-Disk/Folder/foundation-sunshine/Sunshine-<version>-Darwin-arm64.dmg generated.
```

**Step 4: Verify DMG was created**

Run: `ls -lh Sunshine-*.dmg`
Expected: DMG file with size (typically 20-50 MB)

**Step 5: Test mount the DMG**

Run: `hdiutil attach Sunshine-*.dmg`
Expected: DMG mounts successfully, shows mount point

**Step 6: Inspect DMG contents**

Run: `ls -la /Volumes/Sunshine*/`
Expected: See Sunshine.app and possibly Applications symlink

**Step 7: Unmount DMG**

Run: `hdiutil detach /Volumes/Sunshine*`
Expected: DMG unmounted successfully

**Step 8: Document the package location**

Create a simple note file:

```bash
echo "DMG Package: $(ls Sunshine-*.dmg)" > DMG_PACKAGE_INFO.txt
echo "Created: $(date)" >> DMG_PACKAGE_INFO.txt
echo "Architecture: ARM64 (Apple Silicon)" >> DMG_PACKAGE_INFO.txt
echo "" >> DMG_PACKAGE_INFO.txt
echo "Installation: Drag Sunshine.app to Applications folder" >> DMG_PACKAGE_INFO.txt
echo "Note: Not code-signed. Right-click > Open on first launch." >> DMG_PACKAGE_INFO.txt
```

**Step 9: Commit package info**

```bash
git add DMG_PACKAGE_INFO.txt
git commit -m "build: generate DMG package for macOS distribution

Package ready for user testing on Apple Silicon Macs.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 6: Cleanup Temporary Files

**Files:**
- Delete: 8 temporary MD files
- Delete: Backup files (*.bak*)
- Delete: Test scripts

**Step 1: List temporary MD files to be deleted**

Run: `ls -la *.md | grep -E "(ENCODER|FFMPEG|ROOT|STAGE|MOUSE|WEBUI)"`
Expected: List of 8 temporary MD files

**Step 2: Delete temporary documentation files**

Run:
```bash
rm -f ENCODER_FIX_SUMMARY.md \
      FFMPEG_BUG_REPORT.md \
      ROOT_CAUSE_FIX.md \
      STAGE1_TEST_REPORT.md \
      MACOS_MOUSE_FIX_SUMMARY.md \
      MACOS_MOUSE_BUILD_REPORT.md \
      MOUSE_FIX_SUMMARY.md \
      WEBUI_BUILD_REPORT.md
```
Expected: No output, files deleted

**Step 3: Verify temporary MD files are gone**

Run: `ls -la *.md | grep -E "(ENCODER|FFMPEG|ROOT|STAGE|MOUSE|WEBUI)"`
Expected: No output (files deleted)

**Step 4: List backup files**

Run: `find . -name "*.bak*" -type f`
Expected: List of src/video.cpp.bak* files

**Step 5: Delete backup files**

Run: `find . -name "*.bak*" -type f -delete`
Expected: No output, files deleted

**Step 6: Verify backup files are gone**

Run: `find . -name "*.bak*" -type f`
Expected: No output

**Step 7: Delete test scripts**

Run: `rm -f test_encoder_fix.sh test_videotoolbox.sh`
Expected: No output

**Step 8: Verify test scripts are gone**

Run: `ls -la test_*.sh`
Expected: `ls: test_*.sh: No such file or directory`

**Step 9: Check git status**

Run: `git status`
Expected: Shows deleted files

**Step 10: Commit cleanup**

```bash
git add -u
git commit -m "chore: remove temporary debugging files and backups

Removed:
- 8 temporary MD documentation files
- src/video.cpp backup files
- Temporary test scripts

All valuable information has been consolidated into official docs.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 7: Final Validation

**Files:**
- Verify: DMG package
- Verify: Documentation completeness
- Verify: Repository cleanliness

**Step 1: Verify DMG package exists and is valid**

Run: `ls -lh Sunshine-*.dmg && file Sunshine-*.dmg`
Expected: DMG file exists, identified as "VAX COFF executable"

**Step 2: Test DMG installation process**

Run:
```bash
hdiutil attach Sunshine-*.dmg
cp -R /Volumes/Sunshine*/Sunshine.app /tmp/
hdiutil detach /Volumes/Sunshine*
```
Expected: App copied successfully to /tmp

**Step 3: Test launching the app**

Run: `/tmp/Sunshine.app/Contents/MacOS/sunshine --version 2>&1 | head -5`
Expected: Version information displayed (or app starts)

**Step 4: Cleanup test installation**

Run: `rm -rf /tmp/Sunshine.app`
Expected: No output

**Step 5: Verify documentation updates**

Run:
```bash
grep -q "macOS-Specific Issues" docs/troubleshooting.md && echo "✓ troubleshooting.md updated"
grep -q "macOS-Specific Build Notes" docs/building.md && echo "✓ building.md updated"
grep -q "macOS Build Warnings" CLAUDE.md && echo "✓ CLAUDE.md updated"
```
Expected: Three checkmarks

**Step 6: Verify temporary files are deleted**

Run: `ls *.md | grep -E "(ENCODER|FFMPEG|ROOT|STAGE|MOUSE|WEBUI)" | wc -l`
Expected: `0`

**Step 7: Verify no backup files remain**

Run: `find . -name "*.bak*" -type f | wc -l`
Expected: `0`

**Step 8: Check repository status**

Run: `git status`
Expected: Clean working tree (or only untracked DMG file)

**Step 9: Create final summary**

Run:
```bash
cat > BUILD_COMPLETE.md << 'EOF'
# macOS DMG Package Build - Complete

## Date
$(date +%Y-%m-%d)

## Deliverables

### 1. DMG Package
- File: $(ls Sunshine-*.dmg)
- Size: $(ls -lh Sunshine-*.dmg | awk '{print $5}')
- Architecture: ARM64 (Apple Silicon)
- Status: ✅ Ready for testing

### 2. Documentation Updates
- ✅ docs/troubleshooting.md - Added macOS-specific issues section
- ✅ docs/building.md - Added macOS build notes
- ✅ CLAUDE.md - Added macOS build warnings

### 3. Repository Cleanup
- ✅ Removed 8 temporary MD files
- ✅ Removed backup files (*.bak*)
- ✅ Removed test scripts

## Installation Instructions

1. Download: Sunshine-*.dmg
2. Double-click to mount
3. Drag Sunshine.app to Applications folder
4. Right-click Sunshine.app → Open (first launch only)
5. Follow on-screen setup

## Testing Checklist

- [ ] DMG mounts successfully
- [ ] App installs to /Applications
- [ ] App launches without crashes
- [ ] VideoToolbox encoder initializes
- [ ] Moonlight client can connect
- [ ] Mouse input works (including scroll)
- [ ] Video streaming is smooth

## Notes

- Package is NOT code-signed or notarized
- Requires macOS 11.0+ (Big Sur) and Apple Silicon
- For troubleshooting, see docs/troubleshooting.md

## Next Steps

1. Test installation on clean macOS system
2. Verify streaming with Moonlight client
3. Consider code signing for wider distribution
4. Update release notes if publishing
EOF
```

**Step 10: Review and commit summary**

Run: `cat BUILD_COMPLETE.md`
Expected: See complete summary

```bash
git add BUILD_COMPLETE.md
git commit -m "docs: add build completion summary

DMG package ready for user testing.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Success Criteria

✅ All tasks completed successfully when:

1. **Documentation:** Three doc files updated with macOS-specific information
2. **Build:** Clean Release build completes without errors
3. **Package:** DMG file created and validated
4. **Cleanup:** All 8 temporary MD files deleted
5. **Validation:** DMG installs and app launches successfully
6. **Repository:** Clean git status, all changes committed

## Testing the Final Package

After completing all tasks, perform end-to-end testing:

1. **Mount DMG:** Double-click the DMG file
2. **Install:** Drag Sunshine.app to /Applications
3. **Launch:** Right-click → Open (bypass Gatekeeper)
4. **Verify:** Check logs for "Encoder [videotoolbox] passed"
5. **Connect:** Use Moonlight client to test streaming
6. **Test Input:** Verify mouse and keyboard work correctly

If any issues arise, consult `docs/troubleshooting.md` → "macOS-Specific Issues"
