# Sunshine 基地版

## 🌐 多言語サポート / Multi-language Support

<div align="center">

[![English](https://img.shields.io/badge/English-README.en.md-blue?style=for-the-badge)](README.en.md)
[![中文简体](https://img.shields.io/badge/中文简体-README.zh--CN.md-red?style=for-the-badge)](README.md)
[![Français](https://img.shields.io/badge/Français-README.fr.md-green?style=for-the-badge)](README.fr.md)
[![Deutsch](https://img.shields.io/badge/Deutsch-README.de.md-yellow?style=for-the-badge)](README.de.md)
[![日本語](https://img.shields.io/badge/日本語-README.ja.md-purple?style=for-the-badge)](README.ja.md)

</div>

---

LizardByte/Sunshineをベースにしたフォークで、完全なドキュメントサポートを提供します [Read the Docs](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB)。

**Sunshine-Foundation** はMoonlight用のセルフホスト型ゲームストリームホストです。このフォークバージョンはオリジナルのSunshineに基づき、様々なストリーミング端末とWindowsホスト間のゲームストリーミング体験を向上させることに重点を置いた大幅な改良が加えられています：

### 🌟 コア機能
- **HDR フルパイプラインサポート** - HDR10 (PQ) + HLG デュアルフォーマットエンコーディングとアダプティブメタデータにより、幅広い端末デバイスをカバー
- **仮想ディスプレイ** - 内蔵の仮想ディスプレイ管理により、追加ソフトウェアなしで仮想ディスプレイの作成と管理が可能
- **リモートマイク** - クライアントマイクの受信をサポートし、高音質の音声パススルー機能を提供
- **高度なコントロールパネル** - 直感的なWebコントロールインターフェースで、リアルタイム監視と設定管理を提供
- **低遅延伝送** - 最新のハードウェア能力を活用した最適化されたエンコード処理
- **インテリジェントペアリング** - ペアリングデバイスの対応プロファイルをインテリジェントに管理

### 🎬 HDR フルパイプライン技術アーキテクチャ

**デュアルフォーマット HDR エンコーディング：HDR10 (PQ) + HLG 並列サポート**

従来のストリーミングソリューションは HDR10 (PQ) の絶対輝度マッピングのみをサポートしており、クライアントディスプレイがソース側の EOTF パラメータとピーク輝度を正確に再現することが求められます。受信デバイスの能力が不十分な場合やパラメータが一致しない場合、暗部ディテールの消失やハイライトクリッピングなどのトーンマッピングアーティファクトが発生します。

Foundation Sunshine はエンコーディング層に HLG（Hybrid Log-Gamma、ITU-R BT.2100）サポートを追加しました。この規格は相対輝度マッピングを採用し、以下の技術的優位性を備えています：
- **シーン参照型輝度適応**：HLG は相対輝度カーブに基づいており、ディスプレイ側が自身のピーク輝度に基づいて自動的にトーンマッピングを実行 — 低輝度デバイスでの暗部ディテール保持は PQ を大幅に上回る
- **ハイライト領域のスムーズなロールオフ**：HLG のハイブリッド対数ガンマ伝達関数は高輝度領域で漸進的なロールオフを提供し、PQ のハードクリッピングによるバンディングアーティファクトを回避
- **ネイティブ SDR 後方互換性**：HLG 信号は SDR ディスプレイで標準 BT.709 コンテンツとして直接デコード可能。追加のトーンマッピング処理は不要

**フレームごとの輝度分析とアダプティブメタデータ生成**

エンコーディングパイプラインは GPU 側にリアルタイム輝度分析モジュールを統合し、Compute Shader を介して各フレームに対して以下を実行します：
- **MaxFALL / MaxCLL フレーム単位計算**：フレームレベルの最大コンテンツ輝度（MaxCLL）とフレーム平均輝度（MaxFALL）をリアルタイムで計算し、HEVC/AV1 SEI/OBU メタデータに動的に注入
- **ロバストな外れ値フィルタリング**：パーセンタイルベースの切断戦略により極端な輝度ピクセル（例：鏡面反射ハイライト）を除去。孤立した高輝度点がグローバル輝度参照を引き上げ、画面全体が暗くなることを防止
- **フレーム間指数平滑化**：連続フレームの輝度統計値に EMA（指数移動平均）フィルタリングを適用し、シーン遷移時のメタデータ急変による輝度フリッカーを解消

**完全な HDR メタデータパススルー**

HDR10 静的メタデータ（Mastering Display Info + Content Light Level）、HDR Vivid ダイナミックメタデータ、および HLG 伝送特性識別子の完全なパススルーをサポートし、NVENC / AMF / QSV エンコーダが出力するビットストリームが CTA-861 仕様に準拠した完全な色域・輝度情報を含むことを保証します。これにより、クライアントデコーダがソース側の HDR インテントを正確に再現できます。

### 🖥️ 仮想ディスプレイ統合 (win10 22H2 以降のシステムが必要）
- 動的な仮想ディスプレイの作成と破棄
- カスタム解像度とリフレッシュレートのサポート
- マルチディスプレイ設定管理
- 再起動不要のリアルタイム設定変更


## 推奨されるMoonlightクライアント

最適なストリーミング体験を得るために、以下の最適化されたMoonlightクライアントの使用を推奨します（セット効果を発動）：

### 🖥️ Windows(X86_64, Arm64), MacOS, Linux クライアント
[![Moonlight-PC](https://img.shields.io/badge/Moonlight-PC-red?style=for-the-badge&logo=windows)](https://github.com/qiin2333/moonlight-qt)

### 📱 Androidクライアント
[![威力加强版 Moonlight-Android](https://img.shields.io/badge/威力加强版-Moonlight--Android-green?style=for-the-badge&logo=android)](https://github.com/qiin2333/moonlight-android/releases/tag/shortcut)
[![王冠版 Moonlight-Android](https://img.shields.io/badge/王冠版-Moonlight--Android-blue?style=for-the-badge&logo=android)](https://github.com/WACrown/moonlight-android)

### 📱 iOSクライアント
[![虚空终端 Moonlight-iOS](https://img.shields.io/badge/Voidlink-Moonlight--iOS-lightgrey?style=for-the-badge&logo=apple)](https://github.com/The-Fried-Fish/VoidLink)


### 🛠️ その他のリソース 
[awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

## システム要件


> [!WARNING] 
> これらの表は継続的に更新中です。この情報のみに基づいてハードウェアを購入しないでください。


<table>
    <caption id="minimum_requirements">最小システム要件</caption>
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
        <td>Nvidia: NVENC対応グラフィックカード、参照: <a href="https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new">nvencサポートマトリックス</a></td>
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
        <td rowspan="5">オペレーティングシステム</td>
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
2. 設定で詳細なログレベルを有効にして関連情報を見つける
3. [QQグループに参加してヘルプを入手](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)
4. [二文字を使おう！](https://uuyc.163.com/)

**問題フィードバックタグ：**
- `hdr-support` - HDR関連の問題
- `virtual-display` - 仮想ディスプレイの問題  
- `config-help` - 設定関連の問題

## 📚 開発ドキュメント

- **[ビルド手順](docs/building.md)** - プロジェクトのコンパイルとビルド手順
- **[設定ガイド](docs/configuration.md)** - 実行時設定オプションの説明
- **[WebUI開発](docs/WEBUI_DEVELOPMENT.md)** - Vue 3 + Vite Webインターフェース開発完全ガイド

## コミュニティに参加

ディスカッションとコード貢献を歓迎します！
[![QQグループに参加](https://pub.idqqimg.com/wpa/images/group.png 'QQグループに参加')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

## Star History

[![Star History Chart](https://api.star-history.com/chart?repos=AlkaidLab/foundation-sunshine&type=date&legend=top-left&sealed_token=8GzivsLWTBiHWFj-MfIXqxD6tKYaPkTgNvC2q8IjHD2nbEypOWmB3bwOGTGtsCNg-ZKW0uy10gX845qiIMElcA4v_qbJh8OUYhiWtI0aSCvempCz97-OcUeWNrYRPz_rZ0hy7mb8Hfj8qnuVAOZ-p04lzSPXNOyVbm4U-acAHIqyQTdm8FXY-jrXzArQ)](https://www.star-history.com/?repos=AlkaidLab%2Ffoundation-sunshine&type=date&legend=top-left)

---

**Sunshine基地版 - ゲームストリーミングをよりエレガントに**
```
