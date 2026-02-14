# Sunshine Foundation

## 🌐 Support multilingue / Multi-language Support

<div align="center">

[![English](https://img.shields.io/badge/English-README.en.md-blue?style=for-the-badge)](README.en.md)
[![中文简体](https://img.shields.io/badge/简体中文-README.zh--CN.md-red?style=for-the-badge)](README.md)
[![Français](https://img.shields.io/badge/Français-README.fr.md-green?style=for-the-badge)](README.fr.md)
[![Deutsch](https://img.shields.io/badge/Deutsch-README.de.md-yellow?style=for-the-badge)](README.de.md)
[![日本語](https://img.shields.io/badge/日本語-README.ja.md-purple?style=for-the-badge)](README.ja.md)

</div>

---

Fork basé sur LizardByte/Sunshine, offrant une documentation complète [Lire la documentation](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB).

**Sunshine-Foundation** est un hôte de streaming de jeu auto-hébergé pour Moonlight. Cette version forkée apporte des améliorations significatives par rapport à Sunshine original, en se concentrant sur l'amélioration de l'expérience de streaming de jeu entre divers appareils terminaux et l'hôte Windows :

### 🌟 Fonctionnalités principales
- **Support HDR Full Pipeline** - Double encodage HDR10 (PQ) + HLG avec métadonnées adaptatives, couvrant un plus large éventail d'appareils
- **Écran virtuel** - Gestion intégrée des écrans virtuels, permettant de créer et gérer des écrans virtuels sans logiciel supplémentaire
- **Microphone distant** - Prise en charge de la réception du microphone client, offrant une fonction de transmission vocale de haute qualité
- **Panneau de contrôle avancé** - Interface de contrôle Web intuitive avec surveillance en temps réel et gestion de configuration
- **Transmission à faible latence** - Traitement de codage optimisé exploitant les dernières capacités matérielles
- **Appairage intelligent** - Gestion intelligente des profils correspondants aux appareils appairés

### 🎬 Architecture complète du pipeline HDR

**Double format d'encodage HDR : HDR10 (PQ) + HLG en parallèle**

Les solutions de streaming conventionnelles ne prennent en charge que le HDR10 (PQ) avec mappage de luminance absolue, ce qui exige que l'écran client reproduise précisément les paramètres EOTF et la luminosité maximale de la source. Lorsque les capacités de l'appareil récepteur sont insuffisantes ou que les paramètres de luminosité ne correspondent pas, des artefacts de tone mapping apparaissent, tels que la perte de détails dans les ombres et l'écrêtage des hautes lumières.

Foundation Sunshine introduit la prise en charge du HLG (Hybrid Log-Gamma, ITU-R BT.2100) au niveau de l'encodage. Ce standard utilise un mappage de luminance relatif avec les avantages techniques suivants :
- **Adaptation de luminance référencée à la scène** : Le HLG utilise une courbe de luminance relative, permettant à l'écran d'effectuer automatiquement le tone mapping en fonction de sa propre luminosité maximale — la préservation des détails d'ombre sur les appareils à faible luminosité est significativement supérieure au PQ
- **Roll-off progressif des hautes lumières** : La fonction de transfert log-gamma hybride du HLG fournit un roll-off graduel dans les zones de haute luminosité, évitant les artefacts de banding causés par l'écrêtage dur du PQ
- **Compatibilité ascendante native SDR** : Les signaux HLG peuvent être directement décodés par les écrans SDR comme du contenu standard BT.709 sans traitement de tone mapping supplémentaire

**Analyse de luminance image par image et génération adaptative de métadonnées**

Le pipeline d'encodage intègre un module d'analyse de luminance en temps réel côté GPU, exécutant via des Compute Shaders sur chaque image :
- **Calcul MaxFALL / MaxCLL par image** : Calcul en temps réel du niveau de lumière maximal du contenu (MaxCLL) et du niveau de lumière moyen maximal par image (MaxFALL), injectés dynamiquement dans les métadonnées HEVC/AV1 SEI/OBU
- **Filtrage robuste des valeurs aberrantes** : Stratégie de troncature par percentile pour éliminer les pixels de luminance extrême (ex. réflexions spéculaires), empêchant les points lumineux isolés d'élever la référence de luminance globale et de provoquer un assombrissement global de l'image
- **Lissage exponentiel inter-images** : Filtrage EMA (Moyenne Mobile Exponentielle) appliqué aux statistiques de luminance sur les images consécutives, éliminant le scintillement de luminosité causé par les changements brusques de métadonnées lors des transitions de scène

**Transmission complète des métadonnées HDR**

Prise en charge de la transmission complète des métadonnées statiques HDR10 (Mastering Display Info + Content Light Level), des métadonnées dynamiques HDR Vivid et des identifiants de caractéristiques de transfert HLG, garantissant que les flux de bits produits par les encodeurs NVENC / AMF / QSV transportent des informations complètes de volume colorimétrique et de luminance conformes à la spécification CTA-861, permettant aux décodeurs clients de reproduire fidèlement l'intention HDR de la source.

### 🖥️ Intégration d'écran virtuel (nécessite Windows 10 22H2 ou plus récent)
- Création et destruction dynamique d'écrans virtuels
- Prise en charge des résolutions et taux de rafraîchissement personnalisés
- Gestion de configuration multi-écrans
- Modifications de configuration en temps réel sans redémarrage

## Clients Moonlight recommandés

Il est recommandé d'utiliser les clients Moonlight suivants optimisés pour une expérience de streaming optimale (activation des propriétés du set) :

### 🖥️ Clients Windows(X86_64, Arm64), MacOS, Linux
[![Moonlight-PC](https://img.shields.io/badge/Moonlight-PC-red?style=for-the-badge&logo=windows)](https://github.com/qiin2333/moonlight-qt)

### 📱 Client Android
[![Édition renforcée Moonlight-Android](https://img.shields.io/badge/Édition_renforcée-Moonlight--Android-green?style=for-the-badge&logo=android)](https://github.com/qiin2333/moonlight-android/releases/tag/shortcut)
[![Édition Crown Moonlight-Android](https://img.shields.io/badge/Édition_Crown-Moonlight--Android-blue?style=for-the-badge&logo=android)](https://github.com/WACrown/moonlight-android)

### 📱 Client iOS
[![Terminal Void Moonlight-iOS](https://img.shields.io/badge/Voidlink-Moonlight--iOS-lightgrey?style=for-the-badge&logo=apple)](https://github.com/The-Fried-Fish/VoidLink)

### 🛠️ Autres ressources
[awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

## Configuration système requise

> [!WARNING]
> Ces tableaux sont continuellement mis à jour. Veuillez ne pas acheter de matériel uniquement sur la base de ces informations.

<table>
    <caption id="minimum_requirements">Configuration minimale requise</caption>
    <tr>
        <th>Composant</th>
        <th>Exigence</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD : VCE 1.0 ou version ultérieure, voir : <a href="https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support">obs-amd support matériel</a></td>
    </tr>
    <tr>
        <td>Intel : Compatible VAAPI, voir : <a href="https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html">Support matériel VAAPI</a></td>
    </tr>
    <tr>
        <td>Nvidia : Carte graphique supportant NVENC, voir : <a href="https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new">Matrice de support nvenc</a></td>
    </tr>
    <tr>
        <td rowspan="2">CPU</td>
        <td>AMD : Ryzen 3 ou supérieur</td>
    </tr>
    <tr>
        <td>Intel : Core i3 ou supérieur</td>
    </tr>
    <tr>
        <td>RAM</td>
        <td>4GB ou plus</td>
    </tr>
    <tr>
        <td rowspan="5">Système d'exploitation</td>
        <td>Windows : 10 22H2+ (Windows Server ne prend pas en charge les manettes de jeu virtuelles)</td>
    </tr>
    <tr>
        <td>macOS : 12+</td>
    </tr>
    <tr>
        <td>Linux/Debian : 12+ (bookworm)</td>
    </tr>
    <tr>
        <td>Linux/Fedora : 39+</td>
    </tr>
    <tr>
        <td>Linux/Ubuntu : 22.04+ (jammy)</td>
    </tr>
    <tr>
        <td rowspan="2">Réseau</td>
        <td>Hôte : 5GHz, 802.11ac</td>
    </tr>
    <tr>
        <td>Client : 5GHz, 802.11ac</td>
    </tr>
</table>

<table>
    <caption id="4k_suggestions">Configuration recommandée pour la 4K</caption>
    <tr>
        <th>Composant</th>
        <th>Exigence</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD : Video Coding Engine 3.1 ou supérieur</td>
    </tr>
    <tr>
        <td>Intel : HD Graphics 510 ou supérieur</td>
    </tr>
    <tr>
        <td>Nvidia : GeForce GTX 1080 ou modèles supérieurs avec encodeurs multiples</td>
    </tr>
    <tr>
        <td rowspan="2">CPU</td>
        <td>AMD : Ryzen 5 ou supérieur</td>
    </tr>
    <tr>
        <td>Intel : Core i5 ou supérieur</td>
    </tr>
    <tr>
        <td rowspan="2">Réseau</td>
        <td>Hôte : Ethernet CAT5e ou supérieur</td>
    </tr>
    <tr>
        <td>Client : Ethernet CAT5e ou supérieur</td>
    </tr>
</table>

## Support technique

Procédure de résolution des problèmes :
1. Consultez la [documentation d'utilisation](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [Documentation LizardByte](https://docs.lizardbyte.dev/projects/sunshine/latest/)
2. Activez le niveau de journalisation détaillé dans les paramètres pour trouver des informations pertinentes
3. [Rejoignez le groupe QQ pour obtenir de l'aide](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)
4. [Utilisez deux lettres !](https://uuyc.163.com/)

**Étiquettes de signalement des problèmes :**
- `hdr-support` - Problèmes liés au HDR
- `virtual-display` - Problèmes d'écran virtuel
- `config-help` - Problèmes de configuration

## 📚 Documentation de développement

- **[Instructions de compilation](docs/building.md)** - Instructions pour compiler et construire le projet
- **[Guide de configuration](docs/configuration.md)** - Description des options de configuration d'exécution
- **[Développement WebUI](docs/WEBUI_DEVELOPMENT.md)** - Guide complet du développement de l'interface Web Vue 3 + Vite

## Rejoignez la communauté

Nous accueillons favorablement les discussions et les contributions de code !
[![Rejoindre le groupe QQ](https://pub.idqqimg.com/wpa/images/group.png 'Rejoindre le groupe QQ')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

## Historique des stars

[![Graphique d'historique des stars](https://api.star-history.com/svg?repos=qiin2333/Sunshine-Foundation&type=Date)](https://www.star-history.com/#qiin2333/Sunshine-Foundation&Date)

---

**Sunshine Foundation - Rendre le streaming de jeux plus élégant**
```