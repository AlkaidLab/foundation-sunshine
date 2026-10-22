# Foundation Sunshine

[简体中文](../../README.md) · [English](README.en.md) · [Deutsch](README.de.md) · [Français](README.fr.md) · 日本語

<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../../assets/readme/hero-dark.png">
  <img src="../../assets/readme/hero.png" width="800" alt="Foundation Sunshine — 仮想ディスプレイ対応の Windows 向け HDR ゲームストリーミング ホスト（Moonlight 向け Sunshine フォーク）">
</picture>

[![Build](https://img.shields.io/github/actions/workflow/status/AlkaidLab/foundation-sunshine/main.yml?branch=master&label=Build)](https://github.com/AlkaidLab/foundation-sunshine/actions/workflows/main.yml) [![Latest release](https://img.shields.io/github/v/release/AlkaidLab/foundation-sunshine?include_prereleases&sort=date&label=Latest)](https://github.com/AlkaidLab/foundation-sunshine/releases) [![Downloads](https://img.shields.io/github/downloads/AlkaidLab/foundation-sunshine/total?label=Downloads)](https://github.com/AlkaidLab/foundation-sunshine/releases) [![GitHub Stars](https://img.shields.io/github/stars/AlkaidLab/foundation-sunshine?style=flat&label=Stars)](https://github.com/AlkaidLab/foundation-sunshine) [![License](https://img.shields.io/badge/License-GPL--3.0-blue)](https://github.com/AlkaidLab/foundation-sunshine/blob/master/LICENSE)

[公式サイト](https://www.alkaidlab.com/) · [公式サイト（中国）](https://www.alkaidlab.cn/) · [QQ コミュニティ（中国語）](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi) · [問題を報告](https://github.com/AlkaidLab/foundation-sunshine/issues)

</div>

Foundation Sunshine は、Sunshine をベースにした Windows 向けの無料・オープンソースのリモート操作／ゲームストリーミング ホストです。[Sunshine](https://github.com/LizardByte/Sunshine) の拡張フォークで、Moonlight クライアントと組み合わせると、スマートフォン、タブレット、テレビ、Mac などから Windows のデスクトップ、アプリ、PC ゲームを利用できます。

動的 HDR と画面キャリブレーションから低遅延キャプチャ、ハードウェア エンコードまで、Foundation は映像と入力をできるだけローカルに近づけます。サラウンドとマイク転送、DualSense のネイティブ フィードバック、クリップボード、共有フォルダーがセッションと並行して利用でき、専用のデスクトップ GUI が設定、デバイス コンポーネント、ゲーム ライブラリを一か所にまとめます。

**[ホスト（Sunshine）をダウンロード](https://github.com/AlkaidLab/foundation-sunshine/releases/latest)** · **[クライアント（Moonlight）をダウンロード](#クライアントmoonlight)** · **[インストールと初回接続](#クイック-スタート)**

[特長](#特長) · [ドキュメント（英語）](../en/README.md) · [FAQ（英語）](../en/faq.md) · [開発（英語）](../en/development.md) · [コントリビュート](../contributing.en.md)
- [アップグレード（英語）](../en/upgrading.md)：正式版とテスト版、構成の保存、更新の確認。

## 特長

- **映像とディスプレイ：** シーン適応 HDR、HDR10+ / HDR Vivid、Dolby Vision 拡張、輝度キャリブレーション。[ZakoDisplay](https://github.com/qiin2333/zako-vdd) は物理モニターなしで HDR 仮想ディスプレイと複数の画面レイアウトを提供します。
- **キャプチャとエンコード：** NVIDIA NVENC、AMD AMF、Intel Quick Sync に対応し、低遅延で滑らかなセッション向けに最適化。Zako Direct は仮想ディスプレイのフレームを直接取得します。
- **オーディオとマイク転送：** 最大 7.1.4、PCM / AC3 / E-AC3、クライアントのマイク転送、無音時の連続オーディオ、パケット損失の補償。
- **専用デスクトップ GUI：** ゲーム ライブラリ、ディスプレイ、デバイス コンポーネント、実行状態、更新を管理。ログ診断と任意の AI 支援つき。
- **入力とコントローラー：** 高精度タッチパッド、ペン入力、クライアント側カーソル。デバイス センターで DualSense コンポーネントをインストールすると、対応ゲームのアダプティブ トリガーや触覚フィードバックを遠隔でも利用できます。
- **連携とセッション管理：** 双方向クリップボード、共有フォルダー、USB 転送、セッション中の画質調整、適応ビットレート、ホスト コマンド、セッション制御。

要件と有効化の手順は [features and setup（英語）](../en/features.md) を参照。小さな工夫も多数隠されています。

## ダウンロード

ダウンロードは[公式サイト](https://www.alkaidlab.com/)や[公式サイト（中国）](https://www.alkaidlab.cn/)からも探せます。2 台にそれぞれアプリをインストールします：**操作される PC に Foundation Sunshine、操作する側の機器に Moonlight クライアント。**

### ホスト（Sunshine）

**操作される Windows PC にインストールします。**

Foundation Sunshine が役に立ったら、プロジェクトに [⭐ Star](https://github.com/AlkaidLab/foundation-sunshine) を付けてもらえると嬉しいです。

[最新の正式リリース](https://github.com/AlkaidLab/foundation-sunshine/releases/latest)で **Assets** を開き、`WindowsInstaller.exe` で終わるファイルをダウンロードします。初回はインストーラーを使用し、`Portable-x64.zip` は複数起動などの特別な用途のみ（非推奨）。新機能は[プレリリース](https://github.com/AlkaidLab/foundation-sunshine/releases)で確認できます。

Windows 10 22H2 以降と、ハードウェア ビデオ エンコード対応の AMD / Intel / NVIDIA GPU が必要です。画質、解像度、フレームレートは GPU、ドライバー、ネットワーク、クライアントに依存します。

### クライアント（Moonlight）

**スマートフォン、タブレット、または別のコンピューターにインストールします。**

**以下を推奨します。** Foundation の HDR、ディスプレイ、オーディオ、入力の拡張はクライアント側の対応が必要です。他のクライアントでは機能が欠けたり、互換性の問題が起きる場合があります。これらのクライアントは AlkaidLab メンバーが維持しています。

- **Windows / Linux：** [Moonlight V+ for PC](https://github.com/qiin2333/moonlight-qt/releases)
- **macOS：** [Moonlight macOS Enhanced](https://github.com/skyhua0224/moonlight-macos-enhanced/releases) · [Moonlight V+ for PC](https://github.com/qiin2333/moonlight-qt/releases)
- **Android / Android TV：** [Moonlight V+](https://github.com/qiin2333/moonlight-vplus/releases/latest)
- **HarmonyOS：** [Moonlight V+（AppGallery）](https://appgallery.huawei.com/app/detail?id=com.alkaidlab.sdream)

#### パートナー クライアント

- **VoidLink · iOS / iPadOS：** [紹介](https://github.com/The-Fried-Fish/VoidLink-previously-moonlight-zwm#readme) · [App Store（中国・韓国・ベトナム）](https://apps.apple.com/app/voidlink/id6747717070) · [App Store（その他の地域）](https://apps.apple.com/app/voidlink-extreme/id6755103808)（ストア配信は有料）
- **VoidLink TestFlight Beta · Apple TV：** [TestFlight に参加](https://testflight.apple.com/join/k7gs61mZ)
- **Crown Edition · Android：** [ダウンロード](https://github.com/WACrown/moonlight-android/releases) · [紹介](https://github.com/WACrown/moonlight-android#readme)
- **Baozi Edition · HarmonyOS：** [プロジェクトとダウンロード](https://gitee.com/smdsbz/moonlight-ohos)

## クイック スタート

1. **ホストをインストール：** リリースからインストーラーをダウンロードして実行します。必要なコンポーネントを選び、仮想ディスプレイを使う場合は **Zako Display Driver** を残します。続いてデスクトップ パネルの案内に従い、言語、GPU、ディスプレイを設定します。物理モニターがない場合は仮想ディスプレイ ドライバーを確認し、基地（Foundation）ディスプレイを選びます。
2. **クライアントをインストール：** 機器に合った Moonlight クライアントをインストールします。詳しい手順は英語ガイドを参照してください。
3. **PC を追加してペアリング：** クライアントで PC を選択します。表示されない場合は `ipconfig` で IPv4 アドレスを確認して手動で追加。クライアントに表示された PIN を Sunshine の Web 画面（**PIN**）で入力します。
4. **最初の画質設定：** 1080p、60 FPS、20 Mbps（ネットワークが不安定なら 10 Mbps）、コーデックは自動、HDR はオフ、オーディオはステレオ / Opus。
5. **まずデスクトップを確認：** マウス、ウィンドウ、音声を確認してから HDR、解像度、仮想ディスプレイを調整します。
6. **ゲームの追加（任意）：** デスクトップ GUI の Steam/Epic/GOG スキャン、または手動でアプリを追加します。

**最初の画質設定：**

| 項目 | 最初の値 | 意味 |
| --- | --- | --- |
| 解像度 | 1920×1080 (1080p) | 伝送画像の大きさ。高いほど鮮明だが、ネットワークと処理能力を消費 |
| フレームレート | 60 FPS | 1 秒あたりの更新回数。120 FPS は両端の画面と余力が必要 |
| ビットレート | 20 Mbps。不安定なら 10 Mbps に下げる | 圧縮品質とネットワーク負荷を決めるデータ量 |
| ビデオ コーデック | 自動。互換性問題時はまず H.264 | 圧縮形式。HEVC / AV1 は対応デコーダーが必要 |
| HDR | 最初はオフ | 既定は SDR。動作確認後に HDR を有効化 |
| オーディオ | ステレオ / Opus | まず音を確認し、その後サラウンド等を有効化 |

## ドキュメントとサポート

- [ドキュメント（英語）](../en/README.md)：インストール、機能、FAQ、開発をタスク別に整理。
- [FAQ（英語）](../en/faq.md)：カクつき、接続、オーディオの問題。
- [公式サイト](https://www.alkaidlab.com/) · [公式サイト（中国）](https://www.alkaidlab.cn/) · [QQ コミュニティ（中国語）](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)。
- [問題を報告](https://github.com/AlkaidLab/foundation-sunshine/issues) · [ディスカッション](https://github.com/AlkaidLab/foundation-sunshine/discussions) · [コントリビュート](../contributing.en.md)。

[LizardByte/Sunshine](https://github.com/LizardByte/Sunshine)、[Moonlight](https://moonlight-stream.org/)、[ZakoDisplay](https://github.com/qiin2333/zako-vdd) とすべてのコントリビューターに感謝します。ライセンス：[GPL-3.0](https://github.com/AlkaidLab/foundation-sunshine/blob/master/LICENSE)。

気に入ったら Star を。べ、別に見てるわけじゃないです。

## AlkaidLab チーム

| [![qiin2333](https://avatars.githubusercontent.com/u/2795904?s=80)](https://github.com/qiin2333) | [![Yundi339](https://avatars.githubusercontent.com/u/38252824?s=80)](https://github.com/Yundi339) | [![ShadowLemoon](https://avatars.githubusercontent.com/u/119576779?s=80)](https://github.com/ShadowLemoon) | [![skyhua0224](https://avatars.githubusercontent.com/u/91328606?s=80)](https://github.com/skyhua0224) |
| :---: | :---: | :---: | :---: |
| [qiin2333](https://github.com/qiin2333) | [Yundi339](https://github.com/Yundi339) | [ShadowLemoon](https://github.com/ShadowLemoon) | [skyhua0224](https://github.com/skyhua0224) |

## その他のコントリビューター

Foundation と上流の Sunshine に貢献してくれた皆さんに感謝します。

<!-- CONTRIBUTORS:START -->
<p align="center">
  <a href="https://github.com/ReenigneArcher"><img src="https://avatars.githubusercontent.com/u/42013603?v=4&amp;s=80" width="40" height="40" alt="ReenigneArcher" title="ReenigneArcher"></a>
  <a href="https://github.com/cgutman"><img src="https://avatars.githubusercontent.com/u/2695644?v=4&amp;s=80" width="40" height="40" alt="cgutman" title="cgutman"></a>
  <a href="https://github.com/apps/dependabot"><img src="https://avatars.githubusercontent.com/in/29110?v=4&amp;s=80" width="40" height="40" alt="dependabot[bot]" title="dependabot[bot]"></a>
  <a href="https://github.com/loki-47-6F-64"><img src="https://avatars.githubusercontent.com/u/3084139?v=4&amp;s=80" width="40" height="40" alt="loki-47-6F-64" title="loki-47-6F-64"></a>
  <a href="https://github.com/ns6089"><img src="https://avatars.githubusercontent.com/u/61738816?v=4&amp;s=80" width="40" height="40" alt="ns6089" title="ns6089"></a>
  <a href="https://github.com/LizardByte-bot"><img src="https://avatars.githubusercontent.com/u/108553330?v=4&amp;s=80" width="40" height="40" alt="LizardByte-bot" title="LizardByte-bot"></a>
  <a href="https://github.com/TheElixZammuto"><img src="https://avatars.githubusercontent.com/u/6505622?v=4&amp;s=80" width="40" height="40" alt="TheElixZammuto" title="TheElixZammuto"></a>
  <a href="https://github.com/psyke83"><img src="https://avatars.githubusercontent.com/u/749000?v=4&amp;s=80" width="40" height="40" alt="psyke83" title="psyke83"></a>
  <a href="https://github.com/istori1"><img src="https://avatars.githubusercontent.com/u/107304850?v=4&amp;s=80" width="40" height="40" alt="istori1" title="istori1"></a>
  <a href="https://github.com/Nonary"><img src="https://avatars.githubusercontent.com/u/27069224?v=4&amp;s=80" width="40" height="40" alt="Nonary" title="Nonary"></a>
  <a href="https://github.com/apps/github-actions"><img src="https://avatars.githubusercontent.com/in/15368?v=4&amp;s=80" width="40" height="40" alt="github-actions[bot]" title="github-actions[bot]"></a>
  <a href="https://github.com/PapyKahan"><img src="https://avatars.githubusercontent.com/u/9347985?v=4&amp;s=80" width="40" height="40" alt="PapyKahan" title="PapyKahan"></a>
  <a href="https://github.com/ABeltramo"><img src="https://avatars.githubusercontent.com/u/1122217?v=4&amp;s=80" width="40" height="40" alt="ABeltramo" title="ABeltramo"></a>
  <a href="https://github.com/FrogTheFrog"><img src="https://avatars.githubusercontent.com/u/22381748?v=4&amp;s=80" width="40" height="40" alt="FrogTheFrog" title="FrogTheFrog"></a>
  <a href="https://github.com/HomerSp"><img src="https://avatars.githubusercontent.com/u/487734?v=4&amp;s=80" width="40" height="40" alt="HomerSp" title="HomerSp"></a>
  <a href="https://github.com/kodemeister"><img src="https://avatars.githubusercontent.com/u/1043953?v=4&amp;s=80" width="40" height="40" alt="kodemeister" title="kodemeister"></a>
  <a href="https://github.com/brad-richardson"><img src="https://avatars.githubusercontent.com/u/5924884?v=4&amp;s=80" width="40" height="40" alt="brad-richardson" title="brad-richardson"></a>
  <a href="https://github.com/Doomsdayrs"><img src="https://avatars.githubusercontent.com/u/38189170?v=4&amp;s=80" width="40" height="40" alt="Doomsdayrs" title="Doomsdayrs"></a>
  <a href="https://github.com/gschintgen"><img src="https://avatars.githubusercontent.com/u/11065156?v=4&amp;s=80" width="40" height="40" alt="gschintgen" title="gschintgen"></a>
  <a href="https://github.com/Logical-sh"><img src="https://avatars.githubusercontent.com/u/5807486?v=4&amp;s=80" width="40" height="40" alt="Logical-sh" title="Logical-sh"></a>
  <a href="https://github.com/Hazer"><img src="https://avatars.githubusercontent.com/u/4046170?v=4&amp;s=80" width="40" height="40" alt="Hazer" title="Hazer"></a>
  <a href="https://github.com/diadatp"><img src="https://avatars.githubusercontent.com/u/4490283?v=4&amp;s=80" width="40" height="40" alt="diadatp" title="diadatp"></a>
  <a href="https://github.com/KuleRucket"><img src="https://avatars.githubusercontent.com/u/737724?v=4&amp;s=80" width="40" height="40" alt="KuleRucket" title="KuleRucket"></a>
  <a href="https://github.com/chewi"><img src="https://avatars.githubusercontent.com/u/35072?v=4&amp;s=80" width="40" height="40" alt="chewi" title="chewi"></a>
  <a href="https://github.com/guanzhangrtk"><img src="https://avatars.githubusercontent.com/u/44019025?v=4&amp;s=80" width="40" height="40" alt="guanzhangrtk" title="guanzhangrtk"></a>
  <a href="https://github.com/nowrep"><img src="https://avatars.githubusercontent.com/u/522831?v=4&amp;s=80" width="40" height="40" alt="nowrep" title="nowrep"></a>
  <a href="https://github.com/sitiom"><img src="https://avatars.githubusercontent.com/u/56180050?v=4&amp;s=80" width="40" height="40" alt="sitiom" title="sitiom"></a>
  <a href="https://github.com/luk1337"><img src="https://avatars.githubusercontent.com/u/6830979?v=4&amp;s=80" width="40" height="40" alt="luk1337" title="luk1337"></a>
  <a href="https://github.com/JacekJagosz"><img src="https://avatars.githubusercontent.com/u/28653965?v=4&amp;s=80" width="40" height="40" alt="JacekJagosz" title="JacekJagosz"></a>
  <a href="https://github.com/jorys-paulin"><img src="https://avatars.githubusercontent.com/u/5084309?v=4&amp;s=80" width="40" height="40" alt="jorys-paulin" title="jorys-paulin"></a>
  <a href="https://github.com/tez011"><img src="https://avatars.githubusercontent.com/u/22161268?v=4&amp;s=80" width="40" height="40" alt="tez011" title="tez011"></a>
  <a href="https://github.com/KiralyCraft"><img src="https://avatars.githubusercontent.com/u/10259742?v=4&amp;s=80" width="40" height="40" alt="KiralyCraft" title="KiralyCraft"></a>
  <a href="https://github.com/pgrunzjr"><img src="https://avatars.githubusercontent.com/u/3165068?v=4&amp;s=80" width="40" height="40" alt="pgrunzjr" title="pgrunzjr"></a>
  <a href="https://github.com/thatsysadmin"><img src="https://avatars.githubusercontent.com/u/65380846?v=4&amp;s=80" width="40" height="40" alt="thatsysadmin" title="thatsysadmin"></a>
  <a href="https://github.com/WisdomCode"><img src="https://avatars.githubusercontent.com/u/26511487?v=4&amp;s=80" width="40" height="40" alt="WisdomCode" title="WisdomCode"></a>
  <a href="https://github.com/safijari"><img src="https://avatars.githubusercontent.com/u/5191844?v=4&amp;s=80" width="40" height="40" alt="safijari" title="safijari"></a>
  <a href="https://github.com/korverdev"><img src="https://avatars.githubusercontent.com/u/84342833?v=4&amp;s=80" width="40" height="40" alt="korverdev" title="korverdev"></a>
  <a href="https://github.com/tgurr"><img src="https://avatars.githubusercontent.com/u/414984?v=4&amp;s=80" width="40" height="40" alt="tgurr" title="tgurr"></a>
  <a href="https://github.com/exetico"><img src="https://avatars.githubusercontent.com/u/3549445?v=4&amp;s=80" width="40" height="40" alt="exetico" title="exetico"></a>
  <a href="https://github.com/BigBrainAFK"><img src="https://avatars.githubusercontent.com/u/7173033?v=4&amp;s=80" width="40" height="40" alt="BigBrainAFK" title="BigBrainAFK"></a>
  <a href="https://github.com/Arbitrate3280"><img src="https://avatars.githubusercontent.com/u/26132870?v=4&amp;s=80" width="40" height="40" alt="Arbitrate3280" title="Arbitrate3280"></a>
  <a href="https://github.com/arnerak"><img src="https://avatars.githubusercontent.com/u/29173710?v=4&amp;s=80" width="40" height="40" alt="arnerak" title="arnerak"></a>
  <a href="https://github.com/detiam"><img src="https://avatars.githubusercontent.com/u/44510779?v=4&amp;s=80" width="40" height="40" alt="detiam" title="detiam"></a>
  <a href="https://github.com/dkgkdfg65"><img src="https://avatars.githubusercontent.com/u/219107372?v=4&amp;s=80" width="40" height="40" alt="dkgkdfg65" title="dkgkdfg65"></a>
  <a href="https://github.com/lns103"><img src="https://avatars.githubusercontent.com/u/49310852?v=4&amp;s=80" width="40" height="40" alt="lns103" title="lns103"></a>
  <a href="https://github.com/okkiv"><img src="https://avatars.githubusercontent.com/u/68665660?v=4&amp;s=80" width="40" height="40" alt="okkiv" title="okkiv"></a>
  <a href="https://github.com/w0utert"><img src="https://avatars.githubusercontent.com/u/30574002?v=4&amp;s=80" width="40" height="40" alt="w0utert" title="w0utert"></a>
  <a href="https://github.com/MiroKaku"><img src="https://avatars.githubusercontent.com/u/50670906?v=4&amp;s=80" width="40" height="40" alt="MiroKaku" title="MiroKaku"></a>
  <a href="https://github.com/TimmyOVO"><img src="https://avatars.githubusercontent.com/u/47740511?v=4&amp;s=80" width="40" height="40" alt="TimmyOVO" title="TimmyOVO"></a>
  <a href="https://github.com/ReeceHumphreys"><img src="https://avatars.githubusercontent.com/u/14209515?v=4&amp;s=80" width="40" height="40" alt="ReeceHumphreys" title="ReeceHumphreys"></a>
  <a href="https://github.com/mcourteaux"><img src="https://avatars.githubusercontent.com/u/845012?v=4&amp;s=80" width="40" height="40" alt="mcourteaux" title="mcourteaux"></a>
  <a href="https://github.com/Doridian"><img src="https://avatars.githubusercontent.com/u/631409?v=4&amp;s=80" width="40" height="40" alt="Doridian" title="Doridian"></a>
  <a href="https://github.com/brownj85"><img src="https://avatars.githubusercontent.com/u/35931888?v=4&amp;s=80" width="40" height="40" alt="brownj85" title="brownj85"></a>
  <a href="https://github.com/felipejfc"><img src="https://avatars.githubusercontent.com/u/285782?v=4&amp;s=80" width="40" height="40" alt="felipejfc" title="felipejfc"></a>
  <a href="https://github.com/CLF184"><img src="https://avatars.githubusercontent.com/u/57824267?v=4&amp;s=80" width="40" height="40" alt="CLF184" title="CLF184"></a>
  <a href="https://github.com/briankendall"><img src="https://avatars.githubusercontent.com/u/7917884?v=4&amp;s=80" width="40" height="40" alt="briankendall" title="briankendall"></a>
  <a href="https://github.com/abusse"><img src="https://avatars.githubusercontent.com/u/1366888?v=4&amp;s=80" width="40" height="40" alt="abusse" title="abusse"></a>
  <a href="https://github.com/perroboc"><img src="https://avatars.githubusercontent.com/u/2722773?v=4&amp;s=80" width="40" height="40" alt="perroboc" title="perroboc"></a>
  <a href="https://github.com/andygrundman"><img src="https://avatars.githubusercontent.com/u/105828?v=4&amp;s=80" width="40" height="40" alt="andygrundman" title="andygrundman"></a>
  <a href="https://github.com/CrazyStevenz"><img src="https://avatars.githubusercontent.com/u/40207026?v=4&amp;s=80" width="40" height="40" alt="CrazyStevenz" title="CrazyStevenz"></a>
  <a href="https://github.com/TrueZhuangJia"><img src="https://avatars.githubusercontent.com/u/78474576?v=4&amp;s=80" width="40" height="40" alt="TrueZhuangJia" title="TrueZhuangJia"></a>
  <a href="https://github.com/xanderfrangos"><img src="https://avatars.githubusercontent.com/u/33106561?v=4&amp;s=80" width="40" height="40" alt="xanderfrangos" title="xanderfrangos"></a>
  <a href="https://github.com/zackelia"><img src="https://avatars.githubusercontent.com/u/31748637?v=4&amp;s=80" width="40" height="40" alt="zackelia" title="zackelia"></a>
  <a href="https://github.com/zanechua"><img src="https://avatars.githubusercontent.com/u/4265429?v=4&amp;s=80" width="40" height="40" alt="zanechua" title="zanechua"></a>
  <a href="https://github.com/KawaiiZapic"><img src="https://avatars.githubusercontent.com/u/45936772?v=4&amp;s=80" width="40" height="40" alt="KawaiiZapic" title="KawaiiZapic"></a>
  <a href="https://github.com/Chang-Jin-Lee"><img src="https://avatars.githubusercontent.com/u/54494793?v=4&amp;s=80" width="40" height="40" alt="Chang-Jin-Lee" title="Chang-Jin-Lee"></a>
  <a href="https://github.com/bushshrub"><img src="https://avatars.githubusercontent.com/u/9291792?v=4&amp;s=80" width="40" height="40" alt="bushshrub" title="bushshrub"></a>
  <a href="https://github.com/RickAndTired"><img src="https://avatars.githubusercontent.com/u/53553229?v=4&amp;s=80" width="40" height="40" alt="RickAndTired" title="RickAndTired"></a>
  <a href="https://github.com/rafaelsgirao"><img src="https://avatars.githubusercontent.com/u/7544832?v=4&amp;s=80" width="40" height="40" alt="rafaelsgirao" title="rafaelsgirao"></a>
  <a href="https://github.com/radugrecu97"><img src="https://avatars.githubusercontent.com/u/39441011?v=4&amp;s=80" width="40" height="40" alt="radugrecu97" title="radugrecu97"></a>
  <a href="https://github.com/PVermeer"><img src="https://avatars.githubusercontent.com/u/23457923?v=4&amp;s=80" width="40" height="40" alt="PVermeer" title="PVermeer"></a>
  <a href="https://github.com/devusb"><img src="https://avatars.githubusercontent.com/u/4951663?v=4&amp;s=80" width="40" height="40" alt="devusb" title="devusb"></a>
  <a href="https://github.com/LXY1226"><img src="https://avatars.githubusercontent.com/u/8820178?v=4&amp;s=80" width="40" height="40" alt="LXY1226" title="LXY1226"></a>
  <a href="https://github.com/sethicis"><img src="https://avatars.githubusercontent.com/u/4684016?v=4&amp;s=80" width="40" height="40" alt="sethicis" title="sethicis"></a>
  <a href="https://github.com/SnowSwordScholar"><img src="https://avatars.githubusercontent.com/u/73566969?v=4&amp;s=80" width="40" height="40" alt="SnowSwordScholar" title="SnowSwordScholar"></a>
  <a href="https://github.com/Max-Might"><img src="https://avatars.githubusercontent.com/u/119780?v=4&amp;s=80" width="40" height="40" alt="Max-Might" title="Max-Might"></a>
  <a href="https://github.com/zoeyjodon"><img src="https://avatars.githubusercontent.com/u/76182954?v=4&amp;s=80" width="40" height="40" alt="zoeyjodon" title="zoeyjodon"></a>
  <a href="https://github.com/skryvel"><img src="https://avatars.githubusercontent.com/u/120495777?v=4&amp;s=80" width="40" height="40" alt="skryvel" title="skryvel"></a>
  <a href="https://github.com/sirius128"><img src="https://avatars.githubusercontent.com/u/68372698?v=4&amp;s=80" width="40" height="40" alt="sirius128" title="sirius128"></a>
  <a href="https://github.com/qingm36"><img src="https://avatars.githubusercontent.com/u/76875055?v=4&amp;s=80" width="40" height="40" alt="qingm36" title="qingm36"></a>
  <a href="https://github.com/manteuffel723"><img src="https://avatars.githubusercontent.com/u/40317957?v=4&amp;s=80" width="40" height="40" alt="manteuffel723" title="manteuffel723"></a>
  <a href="https://github.com/jensaymoo"><img src="https://avatars.githubusercontent.com/u/45825226?v=4&amp;s=80" width="40" height="40" alt="jensaymoo" title="jensaymoo"></a>
  <a href="https://github.com/jcj83429"><img src="https://avatars.githubusercontent.com/u/5402600?v=4&amp;s=80" width="40" height="40" alt="jcj83429" title="jcj83429"></a>
  <a href="https://github.com/iMakeSoftware"><img src="https://avatars.githubusercontent.com/u/20089877?v=4&amp;s=80" width="40" height="40" alt="iMakeSoftware" title="iMakeSoftware"></a>
  <a href="https://github.com/exalented"><img src="https://avatars.githubusercontent.com/u/9328864?v=4&amp;s=80" width="40" height="40" alt="exalented" title="exalented"></a>
  <a href="https://github.com/everything411"><img src="https://avatars.githubusercontent.com/u/32010059?v=4&amp;s=80" width="40" height="40" alt="everything411" title="everything411"></a>
  <a href="https://github.com/brycerocky"><img src="https://avatars.githubusercontent.com/u/56776312?v=4&amp;s=80" width="40" height="40" alt="brycerocky" title="brycerocky"></a>
  <a href="https://github.com/aseligmann"><img src="https://avatars.githubusercontent.com/u/40468411?v=4&amp;s=80" width="40" height="40" alt="aseligmann" title="aseligmann"></a>
  <a href="https://github.com/ahmadalli"><img src="https://avatars.githubusercontent.com/u/2167301?v=4&amp;s=80" width="40" height="40" alt="ahmadalli" title="ahmadalli"></a>
  <a href="https://github.com/aetheryx"><img src="https://avatars.githubusercontent.com/u/25572140?v=4&amp;s=80" width="40" height="40" alt="aetheryx" title="aetheryx"></a>
  <a href="https://github.com/notentered"><img src="https://avatars.githubusercontent.com/u/1017032?v=4&amp;s=80" width="40" height="40" alt="notentered" title="notentered"></a>
  <a href="https://github.com/360sym"><img src="https://avatars.githubusercontent.com/u/63459534?v=4&amp;s=80" width="40" height="40" alt="360sym" title="360sym"></a>
  <a href="https://github.com/d-s-x"><img src="https://avatars.githubusercontent.com/u/5136335?v=4&amp;s=80" width="40" height="40" alt="d-s-x" title="d-s-x"></a>
  <a href="https://github.com/Xythus"><img src="https://avatars.githubusercontent.com/u/1338909?v=4&amp;s=80" width="40" height="40" alt="Xythus" title="Xythus"></a>
  <a href="https://github.com/Crashdummyy"><img src="https://avatars.githubusercontent.com/u/15139403?v=4&amp;s=80" width="40" height="40" alt="Crashdummyy" title="Crashdummyy"></a>
  <a href="https://github.com/cgarst"><img src="https://avatars.githubusercontent.com/u/125395?v=4&amp;s=80" width="40" height="40" alt="cgarst" title="cgarst"></a>
  <a href="https://github.com/apps/copilot-swe-agent"><img src="https://avatars.githubusercontent.com/in/1143301?v=4&amp;s=80" width="40" height="40" alt="Copilot" title="Copilot"></a>
  <a href="https://github.com/CodyManess"><img src="https://avatars.githubusercontent.com/u/47895180?v=4&amp;s=80" width="40" height="40" alt="CodyManess" title="CodyManess"></a>
  <a href="https://github.com/Clutchnp"><img src="https://avatars.githubusercontent.com/u/105556048?v=4&amp;s=80" width="40" height="40" alt="Clutchnp" title="Clutchnp"></a>
  <a href="https://github.com/cfalas"><img src="https://avatars.githubusercontent.com/u/34488895?v=4&amp;s=80" width="40" height="40" alt="cfalas" title="cfalas"></a>
  <a href="https://github.com/cfajardo"><img src="https://avatars.githubusercontent.com/u/104530852?v=4&amp;s=80" width="40" height="40" alt="cfajardo" title="cfajardo"></a>
  <a href="https://github.com/cazzoo"><img src="https://avatars.githubusercontent.com/u/1876754?v=4&amp;s=80" width="40" height="40" alt="cazzoo" title="cazzoo"></a>
  <a href="https://github.com/caveat-makes-software"><img src="https://avatars.githubusercontent.com/u/59033732?v=4&amp;s=80" width="40" height="40" alt="caveat-makes-software" title="caveat-makes-software"></a>
  <a href="https://github.com/cathyjf"><img src="https://avatars.githubusercontent.com/u/508485?v=4&amp;s=80" width="40" height="40" alt="cathyjf" title="cathyjf"></a>
  <a href="https://github.com/gamozolabs"><img src="https://avatars.githubusercontent.com/u/13505090?v=4&amp;s=80" width="40" height="40" alt="gamozolabs" title="gamozolabs"></a>
  <a href="https://github.com/BeeLeDev"><img src="https://avatars.githubusercontent.com/u/60373320?v=4&amp;s=80" width="40" height="40" alt="BeeLeDev" title="BeeLeDev"></a>
  <a href="https://github.com/angelnu"><img src="https://avatars.githubusercontent.com/u/4406403?v=4&amp;s=80" width="40" height="40" alt="angelnu" title="angelnu"></a>
  <a href="https://github.com/amarshall"><img src="https://avatars.githubusercontent.com/u/153175?v=4&amp;s=80" width="40" height="40" alt="amarshall" title="amarshall"></a>
  <a href="https://github.com/Aaron-Rumpler"><img src="https://avatars.githubusercontent.com/u/80610725?v=4&amp;s=80" width="40" height="40" alt="Aaron-Rumpler" title="Aaron-Rumpler"></a>
  <a href="https://github.com/kelvie"><img src="https://avatars.githubusercontent.com/u/5893?v=4&amp;s=80" width="40" height="40" alt="kelvie" title="kelvie"></a>
  <a href="https://github.com/KarlVogel"><img src="https://avatars.githubusercontent.com/u/4101575?v=4&amp;s=80" width="40" height="40" alt="KarlVogel" title="KarlVogel"></a>
  <a href="https://github.com/justindhillon"><img src="https://avatars.githubusercontent.com/u/145078271?v=4&amp;s=80" width="40" height="40" alt="justindhillon" title="justindhillon"></a>
  <a href="https://github.com/Tea23"><img src="https://avatars.githubusercontent.com/u/1298028?v=4&amp;s=80" width="40" height="40" alt="Tea23" title="Tea23"></a>
  <a href="https://github.com/jhgarner"><img src="https://avatars.githubusercontent.com/u/7400280?v=4&amp;s=80" width="40" height="40" alt="jhgarner" title="jhgarner"></a>
  <a href="https://github.com/insanemal"><img src="https://avatars.githubusercontent.com/u/215062?v=4&amp;s=80" width="40" height="40" alt="insanemal" title="insanemal"></a>
  <a href="https://github.com/Calinou"><img src="https://avatars.githubusercontent.com/u/180032?v=4&amp;s=80" width="40" height="40" alt="Calinou" title="Calinou"></a>
  <a href="https://github.com/hgaiser"><img src="https://avatars.githubusercontent.com/u/716138?v=4&amp;s=80" width="40" height="40" alt="hgaiser" title="hgaiser"></a>
  <a href="https://github.com/Han-cy830"><img src="https://avatars.githubusercontent.com/u/279452837?v=4&amp;s=80" width="40" height="40" alt="Han-cy830" title="Han-cy830"></a>
  <a href="https://github.com/hadicharara"><img src="https://avatars.githubusercontent.com/u/46264409?v=4&amp;s=80" width="40" height="40" alt="hadicharara" title="hadicharara"></a>
  <a href="https://github.com/c3m3gyanesh"><img src="https://avatars.githubusercontent.com/u/11159228?v=4&amp;s=80" width="40" height="40" alt="c3m3gyanesh" title="c3m3gyanesh"></a>
  <a href="https://github.com/himekifee"><img src="https://avatars.githubusercontent.com/u/65988663?v=4&amp;s=80" width="40" height="40" alt="himekifee" title="himekifee"></a>
  <a href="https://github.com/theofficialgman"><img src="https://avatars.githubusercontent.com/u/28281419?v=4&amp;s=80" width="40" height="40" alt="theofficialgman" title="theofficialgman"></a>
  <a href="https://github.com/ekchang"><img src="https://avatars.githubusercontent.com/u/10266208?v=4&amp;s=80" width="40" height="40" alt="ekchang" title="ekchang"></a>
  <a href="https://github.com/e-dong"><img src="https://avatars.githubusercontent.com/u/16567905?v=4&amp;s=80" width="40" height="40" alt="e-dong" title="e-dong"></a>
  <a href="https://github.com/Gara-Dorta"><img src="https://avatars.githubusercontent.com/u/1507774?v=4&amp;s=80" width="40" height="40" alt="Gara-Dorta" title="Gara-Dorta"></a>
  <a href="https://github.com/ehabajmal"><img src="https://avatars.githubusercontent.com/u/33014707?v=4&amp;s=80" width="40" height="40" alt="ehabajmal" title="ehabajmal"></a>
</p>
<!-- CONTRIBUTORS:END -->

[すべてのコントリビューター](https://github.com/AlkaidLab/foundation-sunshine/graphs/contributors)
