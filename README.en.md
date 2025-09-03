# Sunshine Foundation Edition

## 🌐 Multi-language Support

<div align="center">

[![English](https://img.shields.io/badge/English-README.en.md-blue?style=for-the-badge)](README.en.md)
[![简体中文](https://img.shields.io/badge/简体中文-README.zh--CN.md-red?style=for-the-badge)](README.md)
[![Français](https://img.shields.io/badge/Français-README.fr.md-green?style=for-the-badge)](README.fr.md)
[![Deutsch](https://img.shields.io/badge/Deutsch-README.de.md-yellow?style=for-the-badge)](README.de.md)
[![日本語](https://img.shields.io/badge/日本語-README.ja.md-purple?style=for-the-badge)](README.ja.md)

</div>

---

A fork based on LizardByte/Sunshine, providing comprehensive documentation support [Read the Docs](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB).

**Sunshine-Foundation** is a self-hosted game stream host for Moonlight. This forked version introduces significant improvements over the original Sunshine, focusing on enhancing the game streaming experience for various streaming terminal devices connected to a Windows host:

### 🌟 Core Features
- **HDR-Friendly Support** - Optimized HDR processing pipeline for a true HDR game streaming experience
- **Virtual Display** - Built-in virtual display management, allowing creation and management of virtual displays without additional software
- **Remote Microphone** - Supports receiving client microphones, providing high-quality voice passthrough
- **Advanced Control Panel** - Intuitive web control interface with real-time monitoring and configuration management
- **Low-Latency Transmission** - Optimized encoding processing leveraging the latest hardware capabilities
- **Smart Pairing** - Intelligent management of pairing devices with corresponding profiles

### 🖥️ Virtual Display Integration (Requires Windows 10 22H2 or newer)
- Dynamic virtual display creation and destruction
- Custom resolution and refresh rate support
- Multi-display configuration management
- Real-time configuration changes without restarting

## Recommended Moonlight Clients

For the best streaming experience (activating set bonuses), it is recommended to use the following optimized Moonlight clients:

### 🖥️ Windows (X86_64, Arm64), macOS, Linux Clients
[![Moonlight-PC](https://img.shields.io/badge/Moonlight-PC-red?style=for-the-badge&logo=windows)](https://github.com/qiin2333/moonlight-qt)

### 📱 Android Clients
[![Enhanced Edition Moonlight-Android](https://img.shields.io/badge/Enhanced_Edition-Moonlight--Android-green?style=for-the-badge&logo=android)](https://github.com/qiin2333/moonlight-android/releases/tag/shortcut)
[![Crown Edition Moonlight-Android](https://img.shields.io/badge/Crown_Edition-Moonlight--Android-blue?style=for-the-badge&logo=android)](https://github.com/WACrown/moonlight-android)

### 📱 iOS Client
[![Voidlink Moonlight-iOS](https://img.shields.io/badge/Voidlink-Moonlight--iOS-lightgrey?style=for-the-badge&logo=apple)](https://github.com/The-Fried-Fish/VoidLink)

### 🛠️ Additional Resources
[awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

## System Requirements

> [!WARNING]
> These tables are continuously updated. Please do not purchase hardware based solely on this information.

<table>
    <caption id="minimum_requirements">Minimum Requirements</caption>
    <tr>
        <th>Component</th>
        <th>Requirement</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD: VCE 1.0 or later, see: <a href="https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support">obs-amd hardware support</a></td>
    </tr>
    <tr>
        <td>Intel: VAAPI compatible, see: <a href="https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html">VAAPI hardware support</a></td>
    </tr>
    <tr>
        <td>Nvidia: Graphics card with NVENC support, see: <a href="https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new">NVENC support matrix</a></td>
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
        <td rowspan="5">Operating System</td>
        <td>Windows: 10 22H2+ (Windows Server does not support virtual gamepads)</td>
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
    <caption id="4k_suggestions">4K Recommended Configuration</caption>
    <tr>
        <th>Component</th>
        <th>Requirement</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD: Video Coding Engine 3.1 or later</td>
    </tr>
    <tr>
        <td>Intel: HD Graphics 510 or higher</td>
    </tr>
    <tr>
        <td>Nvidia: GeForce GTX 1080 or higher models with multiple encoders</td>
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

Troubleshooting path when encountering issues:
1. Check the [Usage Documentation](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [LizardByte Documentation](https://docs.lizardbyte.dev/projects/sunshine/latest/)
2. Enable detailed log level in settings to find relevant information
3. [Join the QQ group for help](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)
4. [Use two letters!](https://uuyc.163.com/)

**Issue Feedback Labels:**
- `hdr-support` - HDR-related issues
- `virtual-display` - Virtual display issues
- `config-help` - Configuration-related issues

## 📚 Development Documentation

- **[Building Instructions](docs/building.md)** - Project compilation and building instructions
- **[Configuration Guide](docs/configuration.md)** - Runtime configuration options explanation
- **[WebUI Development](docs/WEBUI_DEVELOPMENT.md)** - Complete guide for Vue 3 + Vite web interface development

## Join the Community

We welcome everyone to participate in discussions and contribute code!
[![Join QQ Group](https://pub.idqqimg.com/wpa/images/group.png 'Join QQ Group')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=qiin2333/Sunshine-Foundation&type=Date)](https://www.star-history.com/#qiin2333/Sunshine-Foundation&Date)

---

**Sunshine Foundation Edition - Making Game Streaming More Elegant**
```