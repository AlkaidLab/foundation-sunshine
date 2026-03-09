#!/bin/bash
# 开启 StreamPad 并设为主显
/usr/bin/curl -s -X GET "http://127.0.0.1:55777/api/betterdisplay/set?connected=on&name=StreamPad"
/usr/bin/curl -s -X GET "http://127.0.0.1:55777/api/betterdisplay/set?main=on&name=StreamPad"

# 切换音频输出到 Sunshine Multi-Output
/opt/homebrew/bin/SwitchAudioSource -s "Sunshine Multi-Output" -t output

# 后台延迟断开 Kuycon 信号
(/usr/bin/curl -s -X GET "http://127.0.0.1:55777/api/betterdisplay/set?connected=off&name=Kuycon%20P32U") &

exit 0
