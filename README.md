# Foundation Sunshine

## 🌐 多语言支持 / Multi-language Support

<div align="center">

[![English](https://img.shields.io/badge/English-README.en.md-blue?style=for-the-badge)](README.en.md)
[![中文简体](https://img.shields.io/badge/中文简体-README.zh--CN.md-red?style=for-the-badge)](README.md)
[![Français](https://img.shields.io/badge/Français-README.fr.md-green?style=for-the-badge)](README.fr.md)
[![Deutsch](https://img.shields.io/badge/Deutsch-README.de.md-yellow?style=for-the-badge)](README.de.md)
[![日本語](https://img.shields.io/badge/日本語-README.ja.md-purple?style=for-the-badge)](README.ja.md)

</div>

---


基于LizardByte/Sunshine的分支，提供完整的文档支持 [Read the Docs](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB)。

**Foundation Sunshine**  is a self-hosted game stream host for Moonlight，本分支版本在原始Sunshine基础上进行了重大改进，专注于提高各种串流终端设备与windows主机接入的游戏串流体验：

### 🌟 核心特性
- **HDR 全链路支持** - 同时支持 HDR10 (PQ) 和 HLG 两种 HDR 格式，覆盖更多终端设备
- **虚拟显示器** - 内置虚拟显示器管理，无需额外软件即可创建和管理虚拟显示器
- **远程麦克风** - 支持接收客户端麦克风，提供高音质的语音直通功能
- **高级控制面板** - 直观的Web控制界面，提供实时监控和配置管理
- **低延迟传输** - 结合最新硬件能力优化的编码处理
- **智能配对** - 智能管理配对设备的对应配置文件

### 🎬 HDR 全链路技术方案

**双格式 HDR 编码：HDR10 (PQ) + HLG 并行支持**

传统串流方案仅支持 HDR10 (PQ) 绝对亮度映射，要求客户端显示设备精确匹配源端 EOTF 参数与峰值亮度。当终端设备能力不足或亮度参数不匹配时，会出现暗部细节丢失、高光截断等色调映射失真问题。

Foundation Sunshine 在编码层新增 HLG（Hybrid Log-Gamma, ITU-R BT.2100）支持，该标准采用相对亮度映射，具备以下技术优势：
- **场景参考式亮度适配**：HLG 基于相对亮度曲线，显示端根据自身峰值亮度自动进行色调映射，低亮度设备上暗部细节保留显著优于 PQ
- **高光区域平滑滚降**：HLG 的对数-伽马混合传输函数在高亮区域提供渐进式滚降，避免 PQ 硬截断导致的高光色阶断裂
- **天然 SDR 向后兼容**：HLG 信号可直接被 SDR 显示器解码为标准 BT.709 画面，无需额外的色调映射处理

**逐帧亮度分析与自适应元数据生成**

编码管线在 GPU 端集成实时亮度分析模块，通过 Compute Shader 对每帧画面执行：
- **MaxFALL / MaxCLL 逐帧计算**：实时统计帧级最大内容亮度（MaxCLL）和帧平均亮度（MaxFALL），动态注入 HEVC/AV1 SEI/OBU 元数据
- **异常值鲁棒过滤**：采用百分位截断策略剔除极端亮度像素（如高光镜面反射），防止孤立高亮点拉高全局亮度参考导致整体画面偏暗
- **帧间指数平滑**：对连续帧的亮度统计值应用 EMA（指数移动平均）滤波，消除场景切换时元数据突变引发的亮度闪烁

**完整 HDR 元数据透传**

支持 HDR10 静态元数据（Mastering Display Info + Content Light Level）、HDR Vivid 动态元数据及 HLG 传输特性标识的完整透传，确保 NVENC / AMF / QSV 编码输出的码流携带符合 CTA-861 规范的完整色彩容积与亮度信息，使客户端解码器能够精确还原源端 HDR 意图。

### 🖥️ 虚拟显示器集成 (需win10 22H2 及更新的系统）
- 自定义分辨率和刷新率支持
- 多显示器组合配置管理
- 无需重启的实时配置更改


## 推荐的Moonlight客户端

建议使用以下经过优化的Moonlight客户端获得最佳的串流体验（激活套装属性）：

### 🖥️ Windows(X86_64, Arm64), MacOS, Linux 客户端
[![Moonlight-PC](https://img.shields.io/badge/Moonlight-PC-red?style=for-the-badge&logo=windows)](https://github.com/qiin2333/moonlight-qt)

### 📱 Android客户端
[![VPLUS Moonlight-Android](https://img.shields.io/badge/威力加强版-Moonlight--Android-green?style=for-the-badge&logo=android)](https://github.com/qiin2333/moonlight-vplus)
[![王冠版 Moonlight-Android](https://img.shields.io/badge/王冠版-Moonlight--Android-blue?style=for-the-badge&logo=android)](https://github.com/WACrown/moonlight-android)

### 📱 iOS客户端
[![虚空终端 Moonlight-iOS](https://img.shields.io/badge/Voidlink-Moonlight--iOS-lightgrey?style=for-the-badge&logo=apple)](https://github.com/The-Fried-Fish/VoidLink-previously-moonlight-zwm)


### 🛠️ 其他资源 
[awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

## 系统要求


> [!WARNING] 
> 这些表格正在持续更新中。请不要仅基于此信息购买硬件。


<table>
    <caption id="minimum_requirements">最低配置要求</caption>
    <tr>
        <th>组件</th>
        <th>要求</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD: VCE 1.0或更高版本，参见: <a href="https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support">obs-amd硬件支持</a></td>
    </tr>
    <tr>
        <td>Intel: VAAPI兼容，参见: <a href="https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html">VAAPI硬件支持</a></td>
    </tr>
    <tr>
        <td>Nvidia: 支持NVENC的显卡，参见: <a href="https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new">nvenc支持矩阵</a></td>
    </tr>
    <tr>
        <td rowspan="2">CPU</td>
        <td>AMD: Ryzen 3或更高</td>
    </tr>
    <tr>
        <td>Intel: Core i3或更高</td>
    </tr>
    <tr>
        <td>RAM</td>
        <td>4GB或更多</td>
    </tr>
    <tr>
        <td rowspan="5">操作系统</td>
        <td>Windows: 10 22H2+ (Windows Server不支持虚拟游戏手柄)</td>
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
        <td rowspan="2">网络</td>
        <td>主机: 5GHz, 802.11ac</td>
    </tr>
    <tr>
        <td>客户端: 5GHz, 802.11ac</td>
    </tr>
</table>

<table>
    <caption id="4k_suggestions">4K推荐配置</caption>
    <tr>
        <th>组件</th>
        <th>要求</th>
    </tr>
    <tr>
        <td rowspan="3">GPU</td>
        <td>AMD: Video Coding Engine 3.1或更高</td>
    </tr>
    <tr>
        <td>Intel: HD Graphics 510或更高</td>
    </tr>
    <tr>
        <td>Nvidia: GeForce GTX 1080或更高的具有多编码器的型号</td>
    </tr>
    <tr>
        <td rowspan="2">CPU</td>
        <td>AMD: Ryzen 5或更高</td>
    </tr>
    <tr>
        <td>Intel: Core i5或更高</td>
    </tr>
    <tr>
        <td rowspan="2">网络</td>
        <td>主机: CAT5e以太网或更好</td>
    </tr>
    <tr>
        <td>客户端: CAT5e以太网或更好</td>
    </tr>
</table>

## 技术支持

遇到问题时的解决路径：
1. 查看 [使用文档](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [LizardByte文档](https://docs.lizardbyte.dev/projects/sunshine/latest/)
2. 在设置中打开详细的日志等级找到相关信息
3. [加入QQ交流群获取帮助](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)
4. [使用两个字母！](https://uuyc.163.com/)

**问题反馈标签：**
- `hdr-support` - HDR相关问题
- `virtual-display` - 虚拟显示器问题  
- `config-help` - 配置相关问题

## 📚 开发文档

- **[构建说明](docs/building.md)** - 项目编译和构建说明
- **[配置指南](docs/configuration.md)** - 运行时配置选项说明
- **[WebUI开发](docs/WEBUI_DEVELOPMENT.md)** - Vue 3 + Vite Web界面开发完整指南

## 加入社区

我们欢迎大家参与讨论和贡献代码！
[![加入QQ群](https://pub.idqqimg.com/wpa/images/group.png '加入QQ群')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=qiin2333/Sunshine-Foundation&type=Date)](https://www.star-history.com/#qiin2333/Sunshine-Foundation&Date)

---

**Sunshine基地版 - 让游戏串流更优雅**
