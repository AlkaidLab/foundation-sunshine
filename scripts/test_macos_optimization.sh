#!/bin/bash
# Quick Test Script for macOS Input and Audio Optimization
# Run this after restarting macOS

set -e

echo "=== macOS Optimization Quick Test ==="
echo ""

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# 1. Check BlackHole
echo "1. Checking BlackHole driver..."
if system_profiler SPAudioDataType 2>/dev/null | grep -q "BlackHole"; then
    echo -e "${GREEN}✓ BlackHole detected${NC}"
else
    echo -e "${RED}✗ BlackHole not detected - restart macOS${NC}"
    exit 1
fi

# 2. Check configuration
echo ""
echo "2. Checking Sunshine configuration..."
if grep -q "mouse_sensitivity" ~/.config/sunshine/sunshine.conf; then
    SENSITIVITY=$(grep "mouse_sensitivity" ~/.config/sunshine/sunshine.conf | cut -d'=' -f2 | tr -d ' ')
    echo -e "${GREEN}✓ Mouse sensitivity: $SENSITIVITY${NC}"
else
    echo -e "${YELLOW}⚠ Mouse sensitivity not configured${NC}"
fi

if grep -q "virtual_sink" ~/.config/sunshine/sunshine.conf; then
    SINK=$(grep "virtual_sink" ~/.config/sunshine/sunshine.conf | cut -d'=' -f2 | tr -d ' ')
    echo -e "${GREEN}✓ Virtual sink: $SINK${NC}"
else
    echo -e "${YELLOW}⚠ Virtual sink not configured${NC}"
fi

# 3. Check Sunshine binary
echo ""
echo "3. Checking Sunshine binary..."
if [[ -f "build/sunshine" ]]; then
    echo -e "${GREEN}✓ Sunshine binary ready${NC}"
else
    echo -e "${RED}✗ Sunshine binary not found - run: ninja -C build${NC}"
    exit 1
fi

# 4. Start Sunshine
echo ""
echo "4. Starting Sunshine..."
echo -e "${YELLOW}Press Ctrl+C to stop${NC}"
echo ""

# Kill existing sunshine process
pkill -9 sunshine 2>/dev/null || true

# Start sunshine
./build/sunshine
