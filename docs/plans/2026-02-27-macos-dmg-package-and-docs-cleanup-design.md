# macOS DMG Package and Documentation Cleanup - Design Document

## Date
2026-02-27

## Overview
Create a production-ready DMG installer package for macOS and consolidate temporary debugging documentation into official project documentation.

## Background
The Foundation Sunshine project has accumulated several temporary markdown files during debugging and fixing macOS-specific issues (encoder initialization, FFmpeg ABI mismatch, mouse input). These contain valuable information that should be preserved in official documentation before cleanup.

## Goals
1. Generate a DMG installer package for macOS users
2. Consolidate debugging information into official documentation
3. Clean up temporary files and build artifacts
4. Ensure the package is ready for user testing

## Non-Goals
- Windows or Linux packaging (out of scope)
- Automated CI/CD integration (future work)
- Code signing and notarization (requires Apple Developer account)

## Design

### 1. Documentation Consolidation

#### 1.1 Target Documentation Files

**docs/troubleshooting.md** - Add "macOS-Specific Issues" section:
- FFmpeg ABI mismatch issue and solution
  - Symptom: SIGSEGV in av_hwframe_ctx_init
  - Root cause: System Homebrew headers vs bundled FFmpeg libraries
  - Solution: CMake include directory ordering with BEFORE SYSTEM
- Colorspace initialization defaults
  - Symptom: Invalid video pixel format -1
  - Root cause: Uninitialized sunshine_colorspace_t struct
  - Solution: Default values in struct definition
- Mouse input issues
  - Scroll wheel not working: Use kCGScrollEventUnitPixel instead of Line
  - Initial focus delay: Disable event suppression with CGEventSourceSetLocalEventsSuppressionInterval
- VideoToolbox encoder initialization best practices

**docs/building.md** - Add macOS build notes:
- FFmpeg bundled headers must have priority over system headers
- CMake configuration requirements
- Common build errors and solutions
- Testing procedures for macOS

**CLAUDE.md** - Update with:
- macOS-specific build warnings
- Reference to troubleshooting documentation
- Testing workflow for macOS builds

#### 1.2 Information Extraction Strategy

Extract key technical details from temporary files:
- ROOT_CAUSE_FIX.md: FFmpeg ABI mismatch analysis (most comprehensive)
- ENCODER_FIX_SUMMARY.md: Colorspace fix summary
- MACOS_MOUSE_FIX_SUMMARY.md: Mouse input fixes
- STAGE1_TEST_REPORT.md: Test validation results

Discard:
- Redundant information across multiple files
- Debugging process details (keep only solutions)
- Temporary build reports

### 2. Build Process

#### 2.1 Clean Build
```bash
# Remove existing build
rm -rf build

# Reconfigure with Release mode
cmake -B build -G Ninja -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON

# Build
ninja -C build
```

#### 2.2 Validation
```bash
# Run tests
./build/tests/test_sunshine --gtest_filter="ColorspaceTest.*:MacOSEncoderTest.*"

# Verify binary
file build/sunshine
# Expected: Mach-O 64-bit executable arm64
```

### 3. DMG Package Generation

#### 3.1 CPack Configuration
Use existing CPackConfig.cmake with DragNDrop generator:
```bash
cpack -G DragNDrop --config ./build/CPackConfig.cmake
```

#### 3.2 Package Structure
- Sunshine.app application bundle
- Background image (if configured)
- Symbolic link to /Applications for drag-and-drop installation
- README or installation instructions

#### 3.3 Package Naming
Format: `Sunshine-<version>-Darwin-arm64.dmg`
Example: `Sunshine-2026.0224.230256-Darwin-arm64.dmg`

### 4. Cleanup Strategy

#### 4.1 Files to Delete
Temporary documentation:
- ENCODER_FIX_SUMMARY.md
- FFMPEG_BUG_REPORT.md
- ROOT_CAUSE_FIX.md
- STAGE1_TEST_REPORT.md
- MACOS_MOUSE_FIX_SUMMARY.md
- MACOS_MOUSE_BUILD_REPORT.md
- MOUSE_FIX_SUMMARY.md
- WEBUI_BUILD_REPORT.md

Backup files:
- src/video.cpp.bak*

Test scripts:
- test_encoder_fix.sh
- test_videotoolbox.sh

#### 4.2 Files to Keep
- All docs/* official documentation
- Multi-language READMEs (README.md, README.*.md)
- CLAUDE.md (updated)
- DOCKER_README.md
- All source code and configuration files

### 5. Testing Plan

#### 5.1 Build Validation
- Verify clean build completes without errors
- Run unit tests (22 new tests should pass)
- Check binary architecture (ARM64)

#### 5.2 Package Validation
- Verify DMG file is created
- Mount DMG and inspect contents
- Test drag-and-drop installation to /Applications
- Launch Sunshine.app and verify it starts

#### 5.3 Functional Testing
- Start Sunshine server
- Verify encoders initialize (VideoToolbox)
- Test Moonlight client connection
- Verify mouse input works (including scroll wheel)
- Verify video streaming quality

## Implementation Steps

1. **Documentation consolidation** (30 min)
   - Read all temporary MD files
   - Extract key information
   - Update docs/troubleshooting.md
   - Update docs/building.md
   - Update CLAUDE.md

2. **Clean build** (10 min)
   - Remove build directory
   - Reconfigure CMake
   - Build with Ninja
   - Run tests

3. **Generate DMG package** (5 min)
   - Run CPack with DragNDrop generator
   - Verify package creation

4. **Cleanup temporary files** (5 min)
   - Delete temporary MD files
   - Delete backup files
   - Delete test scripts

5. **Validation** (15 min)
   - Test DMG installation
   - Launch and test Sunshine
   - Verify documentation is complete

Total estimated time: ~65 minutes

## Risks and Mitigations

**Risk:** Build fails due to missing dependencies
**Mitigation:** Use existing working build environment, verify dependencies first

**Risk:** DMG generation fails due to CPack configuration issues
**Mitigation:** Review CPackConfig.cmake before running, use existing configuration

**Risk:** Important information lost during documentation consolidation
**Mitigation:** Review all temporary files carefully, keep git history for reference

**Risk:** Package doesn't work on other macOS systems
**Mitigation:** Document system requirements, note that this is for testing (not signed/notarized)

## Success Criteria

1. ✅ DMG package successfully created
2. ✅ Package installs on macOS via drag-and-drop
3. ✅ Sunshine launches and encoders initialize
4. ✅ All temporary MD files deleted
5. ✅ Key information preserved in official documentation
6. ✅ Repository is clean and organized

## Future Enhancements

- Code signing with Apple Developer certificate
- Notarization for Gatekeeper compatibility
- Automated DMG generation in CI/CD
- Universal binary (x86_64 + ARM64)
- Homebrew formula for easier installation
