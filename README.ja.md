# Sunshine 基地版

LizardByte/Sunshineをベースにしたフォーク版で、完全なドキュメントサポートを提供します [Read the Docs](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB)。

**Sunshine-Foundation** はMoonlight向けのセルフホスト型ゲームストリームホストです。このフォーク版はオリジナルのSunshineに重大な改良を加え、様々なストリーミング端末とWindowsホスト間のゲームストリーミング体験を向上させることに焦点を当てています：

### 🌟 コア機能
- **HDRフレンドリーサポート** - 最適化されたHDR処理パイプラインにより、真のHDRゲームストリーミング体験を提供
- **統合仮想ディスプレイ** - 追加ソフトウェア不要で仮想ディスプレイの作成・管理が可能
- **リモートマイク** - クライアントマイクの受信をサポートし、高音質な音声パススルー機能を提供
- **高度なコントロールパネル** - 直感的なWebコントロールインターフェースでリアルタイム監視と設定管理
- **低遅延伝送** - 最新ハードウェア能力を活用した最適化エンコーディング処理
- **インテリジェントペアリング** - ペアリングデバイスに対応するプロファイルをスマートに管理

### 🖥️ 仮想ディスプレイ統合 (win10 22H2以降のシステムが必要）
- 動的な仮想ディスプレイの作成と破棄
- カスタム解像度とリフレッシュレートのサポート
- マルチディスプレイ設定管理
- 再起動不要なリアルタイム設定変更


## 推奨Moonlightクライアント

最適なストリーミング体験を得るためには以下の最適化されたMoonlightクライアントを使用することを推奨します（セット効果発動）：

### 🖥️ Windows(X86_64, Arm64), MacOS, Linux クライアント
[![Moonlight-PC](https://img.shields.io/badge/Moonlight-PC-red?style=for-the-badge&logo=windows)](https://github.com/qiin2333/moonlight-qt)

### 📱 Androidクライアント
[![威力加强版 Moonlight-Android](https://img.shields.io/badge/威力加强版-Moonlight--Android-green?style=for-the-badge&logo=android)](https://github.com/qiin2333/moonlight-android/releases/tag/shortcut)
[![王冠版 Moonlight-Android](https://img.shields.io/badge/王冠版-Moonlight--Android-blue?style=for-the-badge&logo=android)](https://github.com/WACrown/moonlight-android)

### 📱 iOSクライアント
[![真砖家版 Moonlight-iOS](https://img.shields.io/badge/真砖家版-Moonlight--iOS-lightgrey?style=for-the-badge&logo=apple)](https://github.com/TrueZhuangJia/moonlight-ios-NativeMultiTouchPassthrough)


### 🛠️ その他のリソース 
[awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

## システム要件


> [!WARNING] 
> これらの表は継続的に更新中です。この情報のみを基にハードウェアを購入しないでください。


<table>
    <caption id="minimum_requirements">最低システム要件</caption>
    <tr>
        <th>コンポーネント</th>
        <th>要件</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD: VCE 1.0以降、参照: <a href="https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support">obs-amdハードウェアサポート</a></td>
    </tr>
    <tr>
        <td>Intel: VAAPI互換、参照: <a href="https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html">VAAPIハードウェアサポート</a></td>
    </tr>
    <tr>
        <td>Nvidia: NVENC対応GPU、参照: <a href="https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new">nvencサポートマトリックス</a></td>
    </tr>
    <tr>
        <td rowspan="2">CPU</td>
        <td>AMD: Ryzen 3以降</td>
    </tr>
    <tr>
        <td>Intel: Core i3以降</td>
    </tr>
    <tr>
        <td>RAM</td>
        <td>4GB以上</td>
    </tr>
    <tr>
        <td rowspan="5">OS</td>
        <td>Windows: 10 22H2+ (Windows Serverは仮想ゲームパッドをサポートしません)</td>
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
        <td rowspan="2">ネットワーク</td>
        <td>ホスト: 5GHz, 802.11ac</td>
    </tr>
    <tr>
        <td>クライアント: 5GHz, 802.11ac</td>
    </tr>
</table>

<table>
    <caption id="4k_suggestions">4K推奨構成</caption>
    <tr>
        <th>コンポーネント</th>
        <th>要件</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD: Video Coding Engine 3.1以降</td>
    </tr>
    <tr>
        <td>Intel: HD Graphics 510以降</td>
    </tr>
    <tr>
        <td>Nvidia: GeForce GTX 1080以降のマルチエンコーダ対応モデル</td>
    </tr>
    <tr>
        <td rowspan="2">CPU</td>
        <td>AMD: Ryzen 5以降</td>
    </tr>
    <tr>
        <td>Intel: Core i5以降</td>
    </tr>
    <tr>
        <td rowspan="2">ネットワーク</td>
        <td>ホスト: CAT5eイーサネット以上</td>
    </tr>
    <tr>
        <td>クライアント: CAT5eイーサネット以上</td>
    </tr>
</table>

## テクニカルサポート

問題が発生した場合の解決手順：
1. [使用ドキュメント](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [LizardByteドキュメント](https://docs.lizardbyte.dev/projects/sunshine/latest/)を確認
2. 設定で詳細なログレベルを有効にし関連情報を取得
3. [QQグループに参加してヘルプを求める](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)
4. [二文字を使おう！](https://uuyc.163.com/)

**問題報告タグ：**
- `hdr-support` - HDR関連問題
- `virtual-display` - 仮想ディスプレイ問題  
- `config-help` - 設定関連問題

## コミュニティに参加

ディスカッションやコード貢献を歓迎します！
[![QQグループに参加](https://pub.idqqimg.com/wpa/images/group.png 'QQグループに参加')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=qiin2333/Sunshine-Foundation&type=Date)](https://www.star-history.com/#qiin2333/Sunshine-Foundation&Date)

---

**Sunshine基地版 - ゲームストリーミングをよりシンプルに**