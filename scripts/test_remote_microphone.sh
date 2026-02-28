#!/bin/bash
# Remote Microphone Test Script

set -e

echo "=== Remote Microphone Test ==="
echo ""

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# 1. Check platform
if [[ "$(uname)" != "Darwin" ]]; then
    echo -e "${RED}✗ This test is for macOS only${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Running on macOS${NC}"

# 2. Check BlackHole
if system_profiler SPAudioDataType 2>/dev/null | grep -q "BlackHole 2ch"; then
    echo -e "${GREEN}✓ BlackHole 2ch detected${NC}"
else
    echo -e "${RED}✗ BlackHole not found${NC}"
    echo "  Install: brew install blackhole-2ch"
    exit 1
fi

# 3. Check configuration
if grep -q "virtual_sink.*BlackHole" ~/.config/sunshine/sunshine.conf 2>/dev/null; then
    echo -e "${GREEN}✓ virtual_sink configured${NC}"
else
    echo -e "${YELLOW}⚠ virtual_sink not configured${NC}"
    echo "  Add to sunshine.conf: virtual_sink = BlackHole 2ch"
fi

# 4. Check system input
CURRENT_INPUT=$(system_profiler SPAudioDataType 2>/dev/null | grep -A 1 "Default Input Device: Yes" | grep -v "Default Input Device" | awk '{print $1}')
if [[ "$CURRENT_INPUT" == "BlackHole" ]]; then
    echo -e "${GREEN}✓ System input is BlackHole${NC}"
else
    echo -e "${YELLOW}⚠ System input is not BlackHole (current: $CURRENT_INPUT)${NC}"
    echo "  Set in: System Settings → Sound → Input → BlackHole 2ch"
fi

# 5. Check Sunshine process
if pgrep -x sunshine > /dev/null; then
    echo -e "${GREEN}✓ Sunshine is running${NC}"
else
    echo -e "${RED}✗ Sunshine is not running${NC}"
    echo "  Start: ./build/sunshine"
    exit 1
fi

# 6. Monitor logs
echo ""
echo "Monitoring Sunshine logs for remote microphone activity..."
echo "Please connect Moonlight and enable microphone."
echo "Press Ctrl+C to stop."
echo ""

tail -f ~/.config/sunshine/sunshine.log | grep --line-buffered -i "microphone\|blackhole\|audio queue"
