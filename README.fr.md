# Sunshine Version Fondation

Une branche basée sur LizardByte/Sunshine, offrant une documentation complète [Lire la documentation](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB).

**Sunshine-Foundation** est un hôte de streaming de jeu auto-hébergé pour Moonlight. Cette version apporte des améliorations significatives par rapport à la version originale de Sunshine, en se concentrant sur l'amélioration de l'expérience de streaming de jeu entre divers appareils terminaux et un hôte Windows :

### 🌟 Fonctionnalités principales
- **Prise en charge HDR optimisée** - Pipeline de traitement HDR optimisé pour une véritable expérience de streaming HDR
- **Écran virtuel intégré** - Gestion intégrée des écrans virtuels, sans logiciel supplémentaire
- **Microphone distant** - Prise en charge du microphone client avec une qualité audio élevée
- **Panneau de contrôle avancé** - Interface Web intuitive pour la surveillance et la configuration en temps réel
- **Transmission à faible latence** - Optimisation du traitement d'encodage avec les dernières capacités matérielles
- **Appairage intelligent** - Gestion intelligente des profils des appareils appairés

### 🖥️ Intégration d'écran virtuel (nécessite Windows 10 22H2 ou plus récent)
- Création et suppression dynamique d'écrans virtuels
- Prise en charge de résolutions et taux de rafraîchissement personnalisés
- Gestion de configurations multi-écrans
- Modifications de configuration en temps réel sans redémarrage


## Clients Moonlight recommandés

Pour une expérience de streaming optimale, nous recommandons les clients Moonlight suivants (activation des propriétés du pack) :

### 🖥️ Clients Windows(X86_64, Arm64), MacOS, Linux
[![Moonlight-PC](https://img.shields.io/badge/Moonlight-PC-red?style=for-the-badge&logo=windows)](https://github.com/qiin2333/moonlight-qt)

### 📱 Client Android
[![Version renforcée Moonlight-Android](https://img.shields.io/badge/Version_renforcée-Moonlight--Android-green?style=for-the-badge&logo=android)](https://github.com/qiin2333/moonlight-android/releases/tag/shortcut)
[![Version Couronne Moonlight-Android](https://img.shields.io/badge/Version_Couronne-Moonlight--Android-blue?style=for-the-badge&logo=android)](https://github.com/WACrown/moonlight-android)

### 📱 Client iOS
[![Version Expert Moonlight-iOS](https://img.shields.io/badge/Version_Expert-Moonlight--iOS-lightgrey?style=for-the-badge&logo=apple)](https://github.com/TrueZhuangJia/moonlight-ios-NativeMultiTouchPassthrough)


### 🛠️ Autres ressources 
[awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

## Configuration requise


> [!WARNING] 
> Ces tableaux sont mis à jour régulièrement. Ne basez pas vos achats de matériel uniquement sur ces informations.


<table>
    <caption id="minimum_requirements">Configuration minimale requise</caption>
    <tr>
        <th>Composant</th>
        <th>Exigence</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD : VCE 1.0 ou supérieur, voir : <a href="https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support">obs-amd support matériel</a></td>
    </tr>
    <tr>
        <td>Intel : Compatible VAAPI, voir : <a href="https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html">Support matériel VAAPI</a></td>
    </tr>
    <tr>
        <td>Nvidia : Carte graphique avec NVENC, voir : <a href="https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new">Matrice de support NVENC</a></td>
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
    <caption id="4k_suggestions">Configuration recommandée pour 4K</caption>
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
        <td>Nvidia : GeForce GTX 1080 ou modèle supérieur avec encodeurs multiples</td>
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

Voici les étapes à suivre en cas de problème :
1. Consultez la [documentation](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [Documentation LizardByte](https://docs.lizardbyte.dev/projects/sunshine/latest/)
2. Activez le niveau de journalisation détaillé dans les paramètres pour obtenir des informations pertinentes
3. [Rejoignez le groupe QQ pour obtenir de l'aide](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)
4. [Utilisez deux lettres !](https://uuyc.163.com/)

**Étiquettes pour les retours de problèmes :**
- `hdr-support` - Problèmes liés au HDR
- `virtual-display` - Problèmes d'écran virtuel  
- `config-help` - Problèmes de configuration

## Rejoignez la communauté

Nous encourageons les discussions et les contributions !
[![Rejoindre le groupe QQ](https://pub.idqqimg.com/wpa/images/group.png 'Rejoindre le groupe QQ')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

## Historique des étoiles

[![Graphique d'historique des étoiles](https://api.star-history.com/svg?repos=qiin2333/Sunshine-Foundation&type=Date)](https://www.star-history.com/#qiin2333/Sunshine-Foundation&Date)

---

**Sunshine Version Fondation - Rendre le streaming de jeu plus simple**
```