#!/bin/bash
# Test if BlackHole is receiving audio signal

echo "Testing BlackHole audio signal..."
echo "This will record 5 seconds from BlackHole and check for non-zero audio"
echo ""

# Record 5 seconds from BlackHole
sox -t coreaudio "BlackHole 2ch" -t wav /tmp/blackhole_test.wav trim 0 5 2>/dev/null

if [ ! -f /tmp/blackhole_test.wav ]; then
    echo "❌ Failed to record from BlackHole"
    echo "Install sox: brew install sox"
    exit 1
fi

# Check if audio has non-zero samples
MAX_AMPLITUDE=$(sox /tmp/blackhole_test.wav -n stat 2>&1 | grep "Maximum amplitude" | awk '{print $3}')

echo "Maximum amplitude: $MAX_AMPLITUDE"

if [ "$MAX_AMPLITUDE" == "0.000000" ]; then
    echo "❌ No audio signal detected in BlackHole"
else
    echo "✅ Audio signal detected in BlackHole!"
fi

rm -f /tmp/blackhole_test.wav
