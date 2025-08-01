# Sunshine Foundation Edition

A fork based on LizardByte/Sunshine, providing comprehensive documentation support [Read the Docs](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB).

**Sunshine-Foundation** is a self-hosted game stream host for Moonlight. This forked version introduces significant improvements over the original Sunshine, focusing on enhancing the game streaming experience for various client devices connected to Windows hosts:

### 🌟 Core Features
- **HDR-Friendly Support** - Optimized HDR processing pipeline for true HDR game streaming
- **Integrated Virtual Display** - Built-in virtual display management, eliminating the need for additional software
- **Remote Microphone** - Supports client microphone input with high-quality audio passthrough
- **Advanced Control Panel** - Intuitive web-based interface for real-time monitoring and configuration
- **Low-Latency Transmission** - Encoding optimized with the latest hardware capabilities
- **Smart Pairing** - Intelligent management of paired device profiles

### 🖥️ Virtual Display Integration (Requires Windows 10 22H2 or later)
- Dynamic virtual display creation and removal
- Custom resolution and refresh rate support
- Multi-display configuration management
- Real-time changes without requiring restarts


## Recommended Moonlight Clients

For the best streaming experience (activating set bonuses), we recommend the following optimized Moonlight clients:

### 🖥️ Windows (X86_64, Arm64), MacOS, Linux Clients
[![Moonlight-PC](https://img.shields.io/badge/Moonlight-PC-red?style=for-the-badge&logo=windows)](https://github.com/qiin2333/moonlight-qt)

### 📱 Android Clients
[![Enhanced Edition Moonlight-Android](https://img.shields.io/badge/Enhanced_Edition-Moonlight--Android-green?style=for-the-badge&logo=android)](https://github.com/qiin2333/moonlight-android/releases/tag/shortcut)
[![Crown Edition Moonlight-Android](https://img.shields.io/badge/Crown_Edition-Moonlight--Android-blue?style=for-the-badge&logo=android)](https://github.com/WACrown/moonlight-android)

### 📱 iOS Client
[![True Expert Edition Moonlight-iOS](https://img.shields.io/badge/True_Expert_Edition-Moonlight--iOS-lightgrey?style=for-the-badge&logo=apple)](https://github.com/TrueZhuangJia/moonlight-ios-NativeMultiTouchPassthrough)


### 🛠️ Additional Resources 
[awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

## System Requirements


> [!WARNING] 
> These tables are continuously updated. Do not purchase hardware based solely on this information.


<table>
    <caption id="minimum_requirements">Minimum Requirements</caption>
    <tr>
        <th>Component</th>
        <th>Requirements</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD: VCE 1.0 or later, see: <a href="https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support">obs-amd hardware support</a></td>
    </tr>
    <tr>
        <td>Intel: VAAPI-compatible, see: <a href="https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html">VAAPI hardware support</a></td>
    </tr>
    <tr>
        <td>Nvidia: NVENC-supported GPUs, see: <a href="https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new">NVENC support matrix</a></td>
    </tr>
    <tr>
        <td rowspan="2">CPU</td>
        <td>AMD: Ryzen 3 or higher</td>
    </tr>
    <tr>
        <td>Intel: Core i3 or higher</td>
    </tr>
    <tr>
        <td>RAM</td>
        <td>4GB or more</td>
    </tr>
    <tr>
        <td rowspan="5">OS</td>
        <td>Windows: 10 22H2+ (Windows Server does not support virtual game controllers)</td>
    </tr>
    <tr>
        <td>macOS: 12+</td>
    </tr>
    <tr>
        <td>Linux/Debian: 12+ (bookworm)</td>
    </tr>
    <tr>
        <td>Linux/Fedora: 39+</td>
    </tr>
    <tr>
        <td>Linux/Ubuntu: 22.04+ (jammy)</td>
    </tr>
    <tr>
        <td rowspan="2">Network</td>
        <td>Host: 5GHz, 802.11ac</td>
    </tr>
    <tr>
        <td>Client: 5GHz, 802.11ac</td>
    </tr>
</table>

<table>
    <caption id="4k_suggestions">4K Recommended Specifications</caption>
    <tr>
        <th>Component</th>
        <th>Requirements</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD: Video Coding Engine 3.1 or later</td>
    </tr>
    <tr>
        <td>Intel: HD Graphics 510 or higher</td>
    </tr>
    <tr>
        <td>Nvidia: GeForce GTX 1080 or higher with multi-encoder support</td>
    </tr>
    <tr>
        <td rowspan="2">CPU</td>
        <td>AMD: Ryzen 5 or higher</td>
    </tr>
    <tr>
        <td>Intel: Core i5 or higher</td>
    </tr>
    <tr>
        <td rowspan="2">Network</td>
        <td>Host: CAT5e Ethernet or better</td>
    </tr>
    <tr>
        <td>Client: CAT5e Ethernet or better</td>
    </tr>
</table>

## Technical Support

Troubleshooting steps:
1. Check the [User Documentation](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [LizardByte Documentation](https://docs.lizardbyte.dev/projects/sunshine/latest/)
2. Enable detailed logging in settings to find relevant information
3. [Join the QQ group for assistance](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)
4. [Use two letters!](https://uuyc.163.com/)

**Issue Tags:**
- `hdr-support` - HDR-related issues
- `virtual-display` - Virtual display problems  
- `config-help` - Configuration-related questions

## Join the Community

We welcome discussions and contributions!
[![Join QQ Group](https://pub.idqqimg.com/wpa/images/group.png 'Join QQ Group')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=qiin2333/Sunshine-Foundation&type=Date)](https://www.star-history.com/#qiin2333/Sunshine-Foundation&Date)

---

**Sunshine Foundation Edition - Making Game Streaming Simpler**
```