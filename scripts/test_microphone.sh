#!/bin/bash
# Test if microphone data is being received

echo "=== Microphone Data Test ==="
echo "Monitoring Sunshine logs for microphone activity..."
echo "Please speak into your Moonlight client microphone now."
echo ""

# Monitor for 30 seconds
timeout 30 tail -f ~/.config/sunshine/sunshine.log 2>/dev/null | while read line; do
    if echo "$line" | grep -qi "mic\|microphone\|write_mic_data\|Initializing virtual"; then
        echo "[$(date +%H:%M:%S)] $line"
    fi
done

echo ""
echo "Test completed. If you see 'Initializing virtual microphone' above,"
echo "the microphone is working. If not, check Moonlight settings."
