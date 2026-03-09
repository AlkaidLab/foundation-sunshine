#!/bin/bash
# 1. 开启 Kuycon
/usr/bin/curl -s -X GET "http://127.0.0.1:55777/api/betterdisplay/set?connected=on&name=Kuycon%20P32U"
/bin/sleep 1

# 2. 关闭镜像（关键！镜像模式下无法切换主显）
/usr/bin/curl -s "http://127.0.0.1:55777/set?mirror=off&name=StreamPad&targetName=Kuycon%20P32U"
/bin/sleep 1

# 3. 设 Kuycon 为主显
/Applications/BetterDisplay.app/Contents/MacOS/BetterDisplay set -name="Kuycon P32U" -main=on
/usr/bin/curl -s "http://127.0.0.1:55777/set?resolution=1920x1080&name=Kuycon%20P32U"

# 4. 恢复音频输出到 Mac mini 扬声器
/opt/homebrew/bin/SwitchAudioSource -s "Mac mini扬声器" -t output
