# Foundation Sunshine Edition

## 🌐 Mehrsprachige Unterstützung / Multi-language Support

<div align="center">

[![English](https://img.shields.io/badge/English-README.en.md-blue?style=for-the-badge)](README.en.md)
[![中文简体](https://img.shields.io/badge/中文简体-README.md-red?style=for-the-badge)](README.md)
[![Français](https://img.shields.io/badge/Français-README.fr.md-green?style=for-the-badge)](README.fr.md)
[![Deutsch](https://img.shields.io/badge/Deutsch-README.de.md-yellow?style=for-the-badge)](README.de.md)
[![日本語](https://img.shields.io/badge/日本語-README.ja.md-purple?style=for-the-badge)](README.ja.md)

</div>

---

Ein Fork basierend auf LizardByte/Sunshine, bietet vollständige Dokumentationsunterstützung [Read the Docs](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB).

**Sunshine-Foundation** ist ein selbst gehosteter Game-Stream-Host für Moonlight. Diese Fork-Version hat erhebliche Verbesserungen gegenüber dem ursprünglichen Sunshine vorgenommen und konzentriert sich darauf, das Spiel-Streaming-Erlebnis für verschiedene Streaming-Endgeräte und Windows-Hosts zu verbessern:

### 🚀 Schnellstart

1. Laden Sie ein Windows-Installationspaket von den [Foundation Sunshine Releases](https://github.com/AlkaidLab/foundation-sunshine/releases) herunter, installieren Sie es und starten Sie Sunshine. Lesen Sie vor der Wahl einer Vorabversion die Versionshinweise.
2. Öffnen Sie auf dem Host [https://localhost:47990](https://localhost:47990). Erstellen und sichern Sie beim ersten Start Ihre Zugangsdaten; der Browser kann vor dem lokalen selbstsignierten Zertifikat warnen.
3. Fügen Sie in Sunshine eine Anwendung und in Moonlight den Host hinzu. Geben Sie anschließend die von Moonlight angezeigte PIN in Sunshine ein.

Virtuelle Displays, DualSense, USB-Weiterleitung und NVIDIA-Bildverbesserungen benötigen jeweils eigene Treiber oder Komponenten. Prüfen Sie deren Status vor der Aktivierung im Bedienfeld.

### 🌟 Kernfunktionen
- **Vollständige HDR-Pipeline-Unterstützung** - Dualformat HDR10 (PQ) + HLG Kodierung mit adaptiven Metadaten für eine breitere Geräteabdeckung
- **Virtuelle Anzeige** - Integrierte Verwaltung von ZakoVDD-Displays; der zugehörige Treiber wird benötigt
- **Entferntes Mikrofon** - Unterstützt das Empfangen von Client-Mikrofonen und bietet hochwertige Sprachdurchleitung
- **Erweiterte Systemsteuerung** - Intuitive Web-Oberfläche zur Konfiguration mit Echtzeit-Überwachung und Verwaltung
- **Niedrige Latenzübertragung** - Optimierte Encoder-Verarbeitung unter Nutzung der neuesten Hardware-Fähigkeiten
- **Intelligente Paarung** - Intelligentes Management von Profilen für gepaarte Geräte
- **Controller und Geräte** - Globale und anwendungsbezogene Wahl des Gamepad-Typs, optionaler virtueller DualSense mit Audiohaptik und USB-Weiterleitung für gekoppelte Clients
- **NVIDIA-Bildverbesserungen** - Optionales RTX HDR und DLSS NR; DLSS NR unterstützt SDR und natives HDR sowie Umschalten und Skalieren der Verarbeitung während des Streams, sofern Hardware und Komponenten verfügbar sind
- **Streamstatus** - Anzeige bestätigter Dolby Vision Profile 8.1 / 8.4-Ausgabe und Option, den Stream nach Trennung aller Clients zu beenden

### Optionale Funktionen

- **Virtueller DualSense:** Wählen Sie den Gamepad-Typ im Controller-Center des Bedienfelds. Installieren Sie zuvor die optionale DualSense-Komponente; Audiohaptik benötigt zusätzlich USB/IP-Transport und einen kompatiblen Client. Ist die Komponente nicht verfügbar, wird die automatische Gamepad-Auswahl verwendet.
- **NVIDIA-Bildverbesserungen:** Richten Sie die RTX HDR- oder DLSS NR-Komponente in der Bildverbesserungsverwaltung ein und aktivieren Sie sie für eine Anwendung. DLSS NR lässt sich während eines Streams anpassen. Die angezeigte SDR-, HDR- oder Dolby Vision-Ausgabe hängt von der Streamaushandlung und den Gerätefähigkeiten ab.
- **USB-Weiterleitung:** Gekoppelte Clients können die USB-Weiterleitung zur Laufzeit konfigurieren, wenn auf dem Windows-Host eine funktionsfähige USB/IP-Transportkomponente vorhanden ist.

### 🎬 Vollständige HDR-Pipeline-Architektur

**Dual-Format HDR-Kodierung: HDR10 (PQ) + HLG Parallelunterstützung**

Herkömmliche Streaming-Lösungen unterstützen nur HDR10 (PQ) mit absoluter Luminanzzuordnung, was erfordert, dass das Client-Display die EOTF-Parameter und Spitzenhelligkeit der Quelle genau reproduziert. Wenn die Fähigkeiten des Empfangsgeräts unzureichend sind oder die Helligkeitsparameter nicht übereinstimmen, treten Tone-Mapping-Artefakte wie abgeschnittene Schatten und überbelichtete Lichter auf.

Foundation Sunshine führt HLG-Unterstützung (Hybrid Log-Gamma, ITU-R BT.2100) auf der Kodierungsebene ein. Dieser Standard verwendet eine relative Luminanzzuordnung mit folgenden technischen Vorteilen:
- **Szenenreferenzierte Luminanzanpassung**: HLG verwendet eine relative Luminanzkurve, die es dem Display ermöglicht, automatisch Tone Mapping basierend auf seiner eigenen Spitzenhelligkeit durchzuführen — die Erhaltung von Schattendetails auf Geräten mit niedriger Helligkeit ist PQ deutlich überlegen
- **Sanfter Highlight-Roll-Off**: Die hybride Log-Gamma-Transferfunktion von HLG bietet einen graduellen Roll-Off in Highlight-Bereichen und vermeidet die Banding-Artefakte, die durch hartes Clipping bei PQ verursacht werden
- **Native SDR-Abwärtskompatibilität**: HLG-Signale können von SDR-Displays direkt als Standard-BT.709-Inhalt dekodiert werden, ohne zusätzliches Tone Mapping

**Einzelbild-Luminanzanalyse und adaptive Metadatengenerierung**

Die Kodierungspipeline integriert ein Echtzeit-Luminanzanalysemodul auf der GPU-Seite, das über Compute Shader für jedes Einzelbild folgende Operationen ausführt:
- **MaxFALL / MaxCLL Einzelbild-Berechnung**: Echtzeit-Berechnung des maximalen Inhaltslichtpegels (MaxCLL) und des maximalen durchschnittlichen Bildlichtpegels (MaxFALL) auf Einzelbildebene, dynamisch in HEVC/AV1 SEI/OBU-Metadaten injiziert
- **Robuste Ausreißerfilterung**: Perzentilbasierte Abschneidestrategie zur Eliminierung extremer Luminanzpixel (z.B. Spiegelreflexionen), um zu verhindern, dass isolierte Leuchtpunkte die globale Luminanzreferenz anheben und zu einer allgemeinen Bildverdunkelung führen
- **Interframe-Exponentialglättung**: EMA-Filterung (Exponentieller gleitender Durchschnitt) auf Luminanzstatistiken über aufeinanderfolgende Frames, zur Beseitigung von Helligkeitsflimmern durch abrupte Metadatenänderungen bei Szenenwechseln

**Vollständige HDR-Metadaten-Durchleitung**

Unterstützt die vollständige Durchleitung von statischen HDR10-Metadaten (Mastering Display Info + Content Light Level), dynamischen HDR Vivid-Metadaten und HLG-Transfercharakteristik-Kennungen. Dies stellt sicher, dass die von NVENC / AMF / QSV-Encodern ausgegebenen Bitstreams vollständige Farbvolumen- und Luminanzinformationen gemäß der CTA-861-Spezifikation enthalten, sodass Client-Decoder die HDR-Absicht der Quelle präzise reproduzieren können.

### 🖥️ Integriertes virtuelles Display (Erfordert Win10 22H2 oder neuer)
- Dynamische Erstellung und Entfernung virtueller Displays
- Unterstützung für benutzerdefinierte Auflösungen und Bildwiederholraten
- Verwaltung von Mehrfachanzeigekonfigurationen
- Echtzeit-Konfigurationsänderungen ohne Neustart


## Empfohlene Moonlight-Clients

Für das beste Streaming-Erlebnis wird die Verwendung der folgenden optimierten Moonlight-Clients empfohlen (aktiviert Set-Boni):

### 🖥️ Windows(X86_64, Arm64), MacOS, Linux Clients
[![Moonlight-PC](https://img.shields.io/badge/Moonlight-PC-red?style=for-the-badge&logo=windows)](https://github.com/qiin2333/moonlight-qt)

### 📱 Android Client
[![Enhanced Edition Moonlight-Android](https://img.shields.io/badge/Enhanced_Edition-Moonlight--Android-green?style=for-the-badge&logo=android)](https://github.com/qiin2333/moonlight-android/releases/tag/shortcut)
[![Crown Edition Moonlight-Android](https://img.shields.io/badge/Crown_Edition-Moonlight--Android-blue?style=for-the-badge&logo=android)](https://github.com/WACrown/moonlight-android)

### 📱 iOS Client
[![Voidlink Moonlight-iOS](https://img.shields.io/badge/Voidlink-Moonlight--iOS-lightgrey?style=for-the-badge&logo=apple)](https://github.com/The-Fried-Fish/VoidLink)


### 🛠️ Weitere Ressourcen
[awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

## Systemanforderungen


> [!WARNING]
> Diese Tabellen werden kontinuierlich aktualisiert. Bitte kaufen Sie Hardware nicht nur basierend auf diesen Informationen.


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
        <td>Nvidia: Grafikkarte mit NVENC-Unterstützung, siehe: <a href="https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new">NVENC-Unterstützungsmatrix</a></td>
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
    <caption id="4k_suggestions">Empfohlene Konfiguration für 4K</caption>
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
        <td>Nvidia: GeForce GTX 1080 oder höhere Modelle mit mehreren Encodern</td>
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

Lösungsweg bei Problemen:
1. Konsultieren Sie die [Nutzungsdokumentation](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [LizardByte-Dokumentation](https://docs.lizardbyte.dev/projects/sunshine/latest/)
2. Aktivieren Sie den detaillierten Log-Level in den Einstellungen, um relevante Informationen zu finden
3. [Treten Sie der QQ-Gruppe bei, um Hilfe zu erhalten](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)
4. [Benutze zwei Buchstaben!](https://uuyc.163.com/)

**Problemrückmeldung-Labels:**
- `hdr-support` - Probleme im Zusammenhang mit HDR
- `virtual-display` - Probleme mit virtuellen Displays
- `config-help` - Probleme im Zusammenhang mit der Konfiguration

## 📚 Entwicklerdokumentation

- **[Build-Anleitung](docs/building.md)** - Anleitung zum Kompilieren und Erstellen des Projekts
- **[Konfigurationshandbuch](docs/configuration.md)** - Erläuterung der Laufzeit-Konfigurationsoptionen
- **[WebUI-Entwicklung](docs/WEBUI_DEVELOPMENT.md)** - Vollständige Anleitung zur Entwicklung der Vue 3 + Vite Web-Oberfläche

## Community beitreten

Wir begrüßen die Teilnahme an Diskussionen und Code-Beiträgen!
[![QQ-Gruppe beitreten](https://pub.idqqimg.com/wpa/images/group.png 'QQ-Gruppe beitreten')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

## Star-Verlauf

[![Star-Verlauf Diagramm](https://api.star-history.com/chart?repos=AlkaidLab/foundation-sunshine&type=date&legend=top-left&sealed_token=8GzivsLWTBiHWFj-MfIXqxD6tKYaPkTgNvC2q8IjHD2nbEypOWmB3bwOGTGtsCNg-ZKW0uy10gX845qiIMElcA4v_qbJh8OUYhiWtI0aSCvempCz97-OcUeWNrYRPz_rZ0hy7mb8Hfj8qnuVAOZ-p04lzSPXNOyVbm4U-acAHIqyQTdm8FXY-jrXzArQ)](https://www.star-history.com/?repos=AlkaidLab%2Ffoundation-sunshine&type=date&legend=top-left)

---

**Foundation Sunshine Edition - Macht Game-Streaming eleganter**
