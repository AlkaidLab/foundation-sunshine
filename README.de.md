# Sunshine Foundation Edition

Ein Fork basierend auf LizardByte/Sunshine mit vollständiger Dokumentationsunterstützung [Read the Docs](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB).

**Sunshine-Foundation** ist ein selbst gehosteter Game-Stream-Host für Moonlight. Diese Version enthält wesentliche Verbesserungen gegenüber dem originalen Sunshine und konzentriert sich auf die Verbesserung des Game-Streaming-Erlebnisses zwischen verschiedenen Endgeräten und Windows-Hosts:

### 🌟 Kernfunktionen
- **HDR-Unterstützung** - Optimierte HDR-Verarbeitungspipeline für ein echtes HDR-Game-Streaming-Erlebnis
- **Integriertes virtuelles Display** - Eingebaute Verwaltung virtueller Displays, keine zusätzliche Software erforderlich
- **Remote-Mikrofon** - Unterstützung für Client-Mikrofone mit hochwertiger Sprachdurchleitung
- **Erweitertes Bedienfeld** - Intuitive Web-Oberfläche zur Echtzeitüberwachung und Konfiguration
- **Niedrige Latenz** - Optimierte Codierung unter Nutzung moderner Hardware
- **Intelligente Paarung** - Automatische Verwaltung von Geräteprofilen

### 🖥️ Virtuelle Display-Integration (erfordert Windows 10 22H2 oder neuer)
- Dynamische Erstellung und Entfernung virtueller Displays
- Unterstützung für benutzerdefinierte Auflösungen und Bildwiederholraten
- Verwaltung mehrerer Display-Konfigurationen
- Echtzeit-Änderungen ohne Neustart


## Empfohlene Moonlight-Clients

Für das beste Streaming-Erlebnis werden folgende optimierte Moonlight-Clients empfohlen (Aktivierung von Set-Eigenschaften):

### 🖥️ Windows(X86_64, Arm64), MacOS, Linux Clients
[![Moonlight-PC](https://img.shields.io/badge/Moonlight-PC-red?style=for-the-badge&logo=windows)](https://github.com/qiin2333/moonlight-qt)

### 📱 Android-Client
[![Enhanced Edition Moonlight-Android](https://img.shields.io/badge/Enhanced-Edition_Moonlight--Android-green?style=for-the-badge&logo=android)](https://github.com/qiin2333/moonlight-android/releases/tag/shortcut)
[![Crown Edition Moonlight-Android](https://img.shields.io/badge/Crown-Edition_Moonlight--Android-blue?style=for-the-badge&logo=android)](https://github.com/WACrown/moonlight-android)

### 📱 iOS-Client
[![True Expert Edition Moonlight-iOS](https://img.shields.io/badge/True_Expert-Edition_Moonlight--iOS-lightgrey?style=for-the-badge&logo=apple)](https://github.com/TrueZhuangJia/moonlight-ios-NativeMultiTouchPassthrough)


### 🛠️ Weitere Ressourcen 
[awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

## Systemanforderungen


> [!WARNING] 
> Diese Tabellen werden kontinuierlich aktualisiert. Kaufentscheidungen sollten nicht allein darauf basieren.


<table>
    <caption id="minimum_requirements">Mindestanforderungen</caption>
    <tr>
        <th>Komponente</th>
        <th>Anforderung</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD: VCE 1.0 oder höher, siehe: <a href="https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support">obs-amd Hardware-Unterstützung</a></td>
    </tr>
    <tr>
        <td>Intel: VAAPI-kompatibel, siehe: <a href="https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html">VAAPI Hardware-Unterstützung</a></td>
    </tr>
    <tr>
        <td>Nvidia: NVENC-fähige Grafikkarte, siehe: <a href="https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new">NVENC Support-Matrix</a></td>
    </tr>
    <tr>
        <td rowspan="2">CPU</td>
        <td>AMD: Ryzen 3 oder höher</td>
    </tr>
    <tr>
        <td>Intel: Core i3 oder höher</td>
    </tr>
    <tr>
        <td>RAM</td>
        <td>4GB oder mehr</td>
    </tr>
    <tr>
        <td rowspan="5">Betriebssystem</td>
        <td>Windows: 10 22H2+ (Windows Server unterstützt keine virtuellen Gamepads)</td>
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
        <td rowspan="2">Netzwerk</td>
        <td>Host: 5GHz, 802.11ac</td>
    </tr>
    <tr>
        <td>Client: 5GHz, 802.11ac</td>
    </tr>
</table>

<table>
    <caption id="4k_suggestions">4K-Empfehlungen</caption>
    <tr>
        <th>Komponente</th>
        <th>Anforderung</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD: Video Coding Engine 3.1 oder höher</td>
    </tr>
    <tr>
        <td>Intel: HD Graphics 510 oder höher</td>
    </tr>
    <tr>
        <td>Nvidia: GeForce GTX 1080 oder höhere Modelle mit Multi-Encoder</td>
    </tr>
    <tr>
        <td rowspan="2">CPU</td>
        <td>AMD: Ryzen 5 oder höher</td>
    </tr>
    <tr>
        <td>Intel: Core i5 oder höher</td>
    </tr>
    <tr>
        <td rowspan="2">Netzwerk</td>
        <td>Host: CAT5e Ethernet oder besser</td>
    </tr>
    <tr>
        <td>Client: CAT5e Ethernet oder besser</td>
    </tr>
</table>

## Technischer Support

Problemlösungsweg:
1. Konsultieren Sie die [Dokumentation](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [LizardByte-Dokumentation](https://docs.lizardbyte.dev/projects/sunshine/latest/)
2. Aktivieren Sie detaillierte Log-Level in den Einstellungen
3. [Treten Sie der QQ-Gruppe bei](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)
4. [Nutzen Sie zwei Buchstaben!](https://uuyc.163.com/)

**Problemkategorien:**
- `hdr-support` - HDR-bezogene Probleme
- `virtual-display` - Virtuelle Display-Probleme  
- `config-help` - Konfigurationsprobleme

## Community-Beitritt

Wir freuen uns über Diskussionen und Code-Beiträge!
[![QQ-Gruppe beitreten](https://pub.idqqimg.com/wpa/images/group.png 'QQ-Gruppe beitreten')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

## Star-Historie

[![Star-Historie-Diagramm](https://api.star-history.com/svg?repos=qiin2333/Sunshine-Foundation&type=Date)](https://www.star-history.com/#qiin2333/Sunshine-Foundation&Date)

---

**Sunshine Foundation Edition - Game-Streaming vereinfacht**
```