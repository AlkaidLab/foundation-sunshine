# CI 流水线说明

面向贡献者：一个改动会触发哪些检查、怎么强制全量运行、编译缓存怎么用、怎么发布预发布、怎么在本地复现。
工作流是 `.github/workflows/main.yml`（名称 *Build and Release*），辅助脚本在 `scripts/ci/`。

## 每个场景跑什么

`changes` 作业读取改动的文件（pull request 取 `pulls/N/files`，push 取 `compare/<before>...<after>`），
`scripts/ci/classify-changes.py` 决定后面的作业。未知路径一律按 C++ 处理，宁可多构建，不漏构建。

| 场景 | 跑什么 | 跳过什么 |
|---|---|---|
| PR：C++、CMake、子模块、`tools/` 等 | Web UI 4 个分片、Windows 构建 + 测试、VDD/MAT 冒烟 | 打包 |
| PR：安装器、`cmake/packaging`、Panel 固定提交 | 同上，再加 Web UI 构建、安装器 UI、Inno Setup 打包 | — |
| PR：Web UI 源码、npm 清单、locale | Web UI 4 个分片 | Windows 构建 |
| PR：只改 `README.md`、`docs/authority/*`、`assets/*` | 链接检查 + 三个读取这些文件的 Web UI 测试 | 分片、Windows 构建 |
| PR：只改其它文档、图片 | 相对链接/图片检查 | 其余全部 |
| PR：只改其它工作流（`.github/workflows/` 下除 `main.yml` 外的文件、`.github/dependabot.yml`） | actionlint、所有 `run` 块的 `bash -n`、脚本单测 | 其余全部 |
| PR：改了 `main.yml`、`.github/actions`、`scripts/ci` | 静态检查、Web UI 4 个分片、Windows 构建 + 测试（流水线自己的改动必须被真正执行一次）；**不含打包** | 打包 |
| push 到 master | 同一套路由；改代码则构建、测试、打包并上传工件 | 只改文档/工作流时不构建 |
| 手动运行（Run workflow） | 全部 | — |
| 预发布 | 见下文"发布" | — |

被条件跳过的**作业**报告为成功；不使用 `on.paths`，因为被路径过滤跳过的**工作流**会让必需检查一直 Pending
（GitHub Docs，*Troubleshooting required status checks*，源 `github-docs-required-checks-20261115`）。

## 唯一的门禁

`CI gate` 汇总所有作业：成功或被有意跳过都算通过，失败、取消不会被放过。只需要把它设为必需检查。

## 强制全量运行

- Actions → Build and Release → Run workflow：任何分支都跑全部作业。
- 在 PR 里改一个流水线文件（`main.yml`、`.github/actions/*`、`scripts/ci/*`）会跑 Web UI 分片和 Windows 构建 + 测试，但**不会打包**；
  要验证完整的打包流程，用 Run workflow，或者让这个 PR 同时改动安装器相关的文件（`cmake/packaging`、`scripts/*installer*` 等）。
- 文件数达到 API 上限（pull request 3000，push 300）、新建分支、强制推送：分类失败时一律当作"全部改动"。

## 编译缓存

- 保存：只有 master 的 push、手动运行和夜间构建保存（`ccache-windows-<配置指纹>-<run id>`，配置指纹以 `-pinned` 或 `-newest` 结尾；只保留最新一份，旧的由修剪步骤删除，修剪的匹配模式必须带上这个后缀，否则一个都匹配不到）。
  在自己 fork 里运行这个工作流时，fork 里的 pull request 也保存，因为 fork 没有上游 master 的缓存可恢复。
- 恢复：除夜间构建外，所有运行按前缀 `ccache-windows-<配置指纹>-` 恢复最近的一份（指纹见下一节）；夜间构建不恢复，从冷缓存开始编译
  （在 8 个并行运行器上，见下一节）。
  pull request 能读到 master 的缓存
  （缓存按分支隔离，pull request 可读基础分支，源 `github-docs-dependency-caching-20261115`）。
- 命中的前提是编译器路径和系统头文件路径一致：MSYS2 由 setup-msys2 装在 runner 的临时目录（`D:\a\_temp\msys64`），不要改用镜像自带的
  `C:\msys64`。位置一变，所有预处理结果和缓存键都不同，整份缓存失效（实测同一份缓存：临时目录 98% 命中，`C:\msys64` 17%）。
- 依赖模式：Windows 编译设置 `CCACHE_DEPEND=1`，未命中时头文件列表取自编译器自己的 `-MD` 文件，不再多跑一遍预处理，命中耗时不变
  （`ccache-manual-20261115`）。实测：GCC 16.1、12 个 Sparxie 编译单元、4 路并行、每次全新缓存目录，比较 CPU 时间（用户态加内核态；
  共享主机上的墙钟时间波动达 3 倍，不可用），4 次取中位数，直接模式 152 s，依赖模式 134 s，少 12%。
- 工具链步骤对镜像卡顿有重试：setup-msys2 只负责解压和启动（`update: false`），基础系统升级、依赖包安装和包缓存由
  `scripts/ci/msys2-pacman.sh` 完成。2026-11-15 主线运行 43736645013 的 8 个编译分片里 4 个死在 pacman 同步数据库
  （`failed retrieving file 'ucrt64.db' from mirror.msys2.org : Operation too slow`，即 10 秒内一个字节都没收到），
  setup-msys2 自带的 `update: true` 只执行一次 `pacman -Syuu`。现在同步、升级和安装各最多尝试 5 次，间隔 10、20、40、60 秒，
  每次尝试单独限时 15 分钟，超时后清掉 pacman 的锁；每次从官方镜像列表的另一个服务器开始
  （`repo.msys2.org`、`mirror.msys2.org`，再到 Tier 1），不丢弃任何镜像，源 `msys2-mirrors-20261116`、`msys2-mirrorlist-mingw-20261116`、
  `pacman-conf-5-20261116`。只同步 `[msys]` 和 `[ucrt64]` 两个仓库。包文件放进 pacman 缓存目录，用 `actions/cache` 取前缀
  `msys2-pkgs-ucrt64-` 的最新一份（只下载版本变了的包），只有默认分支保存，保存前只留已安装版本的文件；数据库每次现取，
  工具链包固定在 `scripts/ci/msys2-pins.txt`（见下文「工具链固定与升级」）。MSYS2 的位置不变（`D:\a\_temp\msys64`）。
  缓存键带模式前缀（pinned/newest），不固定版本的验证构建不会顶掉固定工具链的缓存。配置指纹不再读仓库数据库，只哈希固定清单。
  验证（fork）：正常、全部镜像断开 100 秒（每次尝试限时 45 秒，第一次超时、第二次换服务器后恢复）都通过。
- 构建标识（提交、分支、渠道、CI 运行）不进入大多数编译单元：版本号只由 `src/build_info.cpp` 读取，
  新提交只会重编译这一个文件和真正改动的文件。

## 工具链固定与升级

Windows 作业的 MSYS2 工具链与关键库固定在 `scripts/ci/msys2-pins.txt`：每行一个包的完整名、版本、包文件名和 sha256，覆盖
gcc、gcc-libs、binutils、CRT、头文件、winpthreads、cmake、ninja、ccache、pkgconf、make、python、cppwinrt、openssl、
curl-winssl、miniupnpc、nlohmann-json、MinHook、libvpl 和 opus。`scripts/ci/msys2-pacman.sh` 从官方镜像下载这些文件、校验摘要后
用 `pacman -U` 一次装入（MSYS2 文档支持用包文件安装指定版本，源 `msys2-package-management-20261116`），并把固定名写进
`IgnorePkg`，任何 `-Syu` 都动不了它们（源 `pacman-conf-5-20261116`）。msys 侧的 git/diffutils/curl 和固定包的传递依赖仍跟仓库
最新，它们不参与链接产物；boost 与 FFmpeg 不来自 MSYS2（预编译件与子模块，各自固定）。编译缓存键由固定清单派生，所以升级固定版本
必然换缓存键，上游的小版本更新则不会。

2026-11-16 的一次未固定升级把 OpenSSL 带到 4.1.0 并打断构建（`ASN1_STRING` 不透明化、`X509_get_subject_name` 加 const、静态
libcrypto 需要链接 bcrypt、主机名校验不再回退 CN），修复见 PR #1289。固定的意义就是让这类大版本更新必须经过人工。

**每周巡检**（`.github/workflows/msys2-toolchain-watch.yml`，周一 03:23 UTC；定时事件只在 master 上运行，源 `gh-events-20261116`）
读仓库数据库与固定版本对比：任何包出现更新的 MAJOR（openssl 还看 MINOR）就用 `GITHUB_TOKEN` 派发一次「不固定版本」的验证构建
（*Build and Release* 的 `toolchain_probe` 输入，装最新包），并打开或更新唯一一个带 `toolchain-watch` 标签的 issue，标题如
「工具链有大版本更新：openssl 4.1 → 4.2」，正文列出全部变化并附验证构建链接。没有大版本变化不发 issue；固定版本追上后，下一次
巡检自动关闭旧 issue。手动 Run workflow 可以在任意分支演练：`simulate-newer` 输入模拟新版本，勾选 `trigger-probe` 连验证构建的
派发一起演练。

**升级步骤**：跑 `python3 scripts/ci/msys2-pins.py bump`（版本与 sha256 取自仓库数据库，直接重写固定清单）→ 核对 diff，按 API
变化适配代码（可参考 PR #1289 对 OpenSSL 4 的处理）→ 跑一次 Windows 构建 → 合并。固定版本覆盖新版本后，巡检会自动关掉跟踪 issue。
## Windows 编译器与发布门禁

- 测什么发什么：pull request、push、夜间构建和发布构建都默认使用 MSYS2 UCRT64 的 Clang + LLD，完整编译、原生测试和 PE 发布门禁跑在 Clang 产物上。
  `CI_TOOLCHAIN=gcc` 或手动运行的 `toolchain=gcc` 可切到 GCC。
- GCC 是回退，由 `.github/workflows/gcc-fallback.yml` 每晚（UTC 21:47，合并到 master 后生效）派发一次 GCC 的完整构建（8 个分片冷编译、全部原生测试、发布门禁、打包）并等它结束；
  失败就打开或更新唯一一个带 `gcc-fallback` 标签的 issue，成功后自动关闭。GCC 构建不保存也不修剪编译缓存（只有主工具链才保存），所以不会挤掉 Clang 的缓存。
- Clang 的编译缓存比 GCC 大：同一棵树，热缓存 Clang 约 680 MB，GCC 约 235 MB。多出来的部分是预编译头：每个目标一份，Clang 的 `.pch` 约 25 MB，ccache 把它们当作普通条目保存；
  GCC 的 `.gch` 不进缓存。ccache 只保留一条（见下一节），远低于 GitHub 的 10 GB 配额；热构建在 99% 命中下，Windows 作业约 9 分钟，不比 GCC 慢。
- Clang、LLD 和与它们版本锁定的 `clang-libs`、`llvm-libs` 固定在 `scripts/ci/msys2-pins-clang.txt`，格式与升级方式同 `msys2-pins.txt`（`msys2-pins.py check/bump` 两个文件一并处理）；只有 Clang 构建才装它们并把它们写进 `IgnorePkg`，GCC 构建不受影响，配置指纹也只在 Clang 构建时计入这个文件。
- 两种编译器都启用 `SUNSHINE_PRECOMPILED_HEADERS=ON`。ccache 同时使用 `CCACHE_DEPEND=1` 和 `CCACHE_SLOPPINESS=pch_defines,time_macros`，配置指纹把编译器和 Clang/LLD 包版本纳入缓存键。
- Windows Release 目标使用 `-ffunction-sections -fdata-sections` 与 `--gc-sections`；发布打包仍先从未 strip 的模块生成 Breakpad 符号，再 strip 发出的模块。LTO 暂不启用，因为本卡没有可重复的构建时间收益测量。
- `sunshine.exe` 的发布门禁检查 PE 导入表只含系统 DLL、`SizeOfStackReserve >= 2 MiB`，并在移除 MSYS2 的 PATH 后运行 `sunshine.exe --version`。这会在原生 Windows 构建和打包前失败，避免把依赖开发环境 DLL 的文件送进发布流程。

## 冷构建：在并行运行器上编译

单个 4 vCPU 运行器从零编译要 27–37 分钟。下面三种情况，`shard` 作业先把编译拆到 8 个运行器上；有缓存可恢复的运行（热路径）不启动它们，
耗时与之前相同：

1. 夜间构建，或勾选了 `sharded_cold` 的手动运行。
2. **没有当前配置指纹的编译缓存**。缓存存在不等于能命中：改了编译参数（例如加 `-g1`）、换了编译器或系统头文件的版本之后，旧缓存几乎全部未命中，
   这一次运行实际是冷构建。`scripts/ci/config-fingerprint.py` 把决定命中的输入哈希成 12 位十六进制：`CMakeLists.txt` 和 `cmake/` 下设置参数、
   定义、目标、依赖的模块（不含 `packaging`、`sources` 和 Linux/macOS/Unix 专用文件）、`build_win` 里 `Configure Windows` 步骤、工具链 action，
  以及固定清单 `scripts/ci/msys2-pins.txt`（Windows 作业用 pacman 装的就是这些固定版本的编译器、CRT 和依赖库）。指纹是缓存键的一部分，
  所以运行只会恢复同一配置的缓存，`cold_plan` 也只看这个前缀（默认分支和当前引用）；上游仓库的更新不进指纹，不会让固定工具链的缓存失效。
3. 这次改动碰到**被广泛包含的头文件**。`scripts/ci/hot-headers.txt` 列出对应目标文件的编译时间占全部编译时间 30% 以上的仓库头文件
   （依据 `ninja -t deps` 和 `shard-weights.ninja_log` 的耗时；`src/thread_safe.h` 占 35%，`src/config.h`、`src/logging.h` 约 45%）。
   改动其中一个，这些目标文件全部未命中，约等于冷编译三分之一以上，超过分片的开销。只有能列出改动文件的事件（pull request、push）才会触发这条规则，
   手动运行和新分支不会。夜间构建把新的测量结果作为工件 `ci-measurements`（`hot-headers.txt` 和 `shard-weights.ninja_log`）保留 14 天，
   头文件包含关系变化后用它更新两个文件。

流程：每个分片按与 `Configure Windows` 相同的参数配置，然后一块一块地领取目标文件：`scripts/ci/shard-targets.py` 按
`scripts/ci/shard-weights.ninja_log` 里的耗时把目标文件切成 32 块等重的块（最重的先领），运行器通过创建 git 引用
`refs/claims/<run>-<attempt>/<块号>` 领块（引用已存在时创建返回 422，脚本再用 GET 确认引用确实存在才跳过这一块；422 但引用不存在时，这一块按固定切分处理，所以每块只会被一个运行器领到），领到就编译，直到没有块可领。
领不到任何块的运行器（比如启动晚于几个快的同伴）直接成功结束，这是正常结果。
hosted runner 的 CPU 型号不统一，同样的工作在慢机器上要多花近一倍时间；按块领取让慢机器自然少编几块，
各分片的结束时间因此接近，不需要事先猜谁快。令牌只有 `scripts/ci/shard-compile.sh` 读取，启动 ninja 前已从环境里删除；
来自 fork 的 pull request 令牌只读，创建引用返回 403，脚本退回固定的蛇形切分（第 k 块给第 k mod N 个运行器，隔一轮反向）。
某个分片失败（运行器中途挂掉、一块没人编译）时，`build_win` 仍然运行（取消除外）：它合并已上传的分片缓存（失败的分片也会上传已编译部分的缓存），再编译缺的目标文件，只是多花时间；
`ci-gate` 在这种情况下只要 `build_win` 成功就通过并给出警告，`build_win` 失败（包括真正的编译错误）则整次运行失败。`shard_cleanup` 作业在分片结束后删掉这些引用。
每个分片把 ccache 目录（十几 MB）作为工件上传；`build_win` 下载全部分片缓存解到同一个目录（ccache 条目按内容寻址，合并就是拷贝），
然后照常 `ninja`，此时编译约 100% 命中，再链接、测试、打包。合并之后 `build_win` 只统计这一步的缓存查询，
未命中超过 5% 时发出警告，提示分片的配置与 `Configure Windows` 已经不一致。

与分片并行的两个作业把原来排在 `build_win` 里的步骤挪了出来：`setup_ui`（安装器 setup UI；上游用固定的下载件时不到 10 秒，
没有匹配的下载件时要从 Panel 子模块构建约 3.5 分钟）和 `adapter_config`（不带 SDK 的适配器配置测试，约 2.5 分钟）。

- 分片不构建 MAT 采集包和 dotnet 发布（`-DSUNSHINE_BUILD_MAT_CAPTURE=OFF`）：它们只是目标文件的前置依赖，不改变任何一条编译命令，
  但每个分片都会各跑一遍（各约 50–70 秒）。
- 分片数 `SHARDS: 8` 和块数 `SHARD_CHUNKS`（默认 32）写在 `cold_plan` 作业里。公开仓库免费计划同时最多 20 个作业
  （源 `github-docs-actions-limits-20261115`），一次分片构建占 8 个。
- 权重文件过期只会让块变得不等重，不会出错；分片的 `.ninja_log` 作为工件 `ninja-log-shard-*` 保留 14 天，
  `scripts/ci/merge-shard-weights.py` 把一次运行的日志合成新的权重文件。
- 同一提交的实测（fork，`windows-latest` 冷缓存，端到端墙钟，含各作业 2.5–3.5 分钟的检出、工具链和 configure）：

| 方案 | 端到端 | 说明 |
|---|---|---|
| 单个运行器 | 31–52 分钟（中位数约 37） | 编译 1711–2865 秒，随分到的 CPU 型号而变 |
| 4 个分片 | 约 24 分钟 | 固定切分，不含打包 |
| 6 个分片 | 约 20–21 分钟 | |
| 8 个分片 | 约 18 分钟 | 合并后的构建步骤 66–90 秒，100% 命中 |
| 8 个分片，按块领取（当前实现） | 19.5 分钟（夜间式，含打包）；pull request 方式 16.5 分钟 | 各分片编译 372–504 秒（固定切分时 429–781 秒） |

墙钟取决于最慢的分片：hosted runner 的 CPU 型号不统一（EPYC 7763、Xeon 8573C、EPYC 9V74、EPYC 9V45 都出现过，同样的分片在 9V45 上
257–273 秒，在 7763 上 517–537 秒），所以分片数越多，越容易有一个分片落在慢机器上。

## 发布

- master 的代码 push 会构建、测试、打包，并把安装包、便携包、校验文件作为工件上传（保留期按 `retention-days`），
  但**默认不发布预发布**。
- 发布一个预发布：Actions → Build and Release → Run workflow → 在 master 上勾选 `publish`。
  构建成功后，`publish` 作业下载本次运行的工件，用 `gh release create --prerelease` 创建发布，
  标签与 push 路径相同（`v年.月日.时分秒`，由提交者时间算出，见 `scripts/ci/release-info.py`），资产挂在发布上。
  只有 `publish` 作业持有 `contents: write`。
- 恢复每次 master push 自动发布：把仓库变量 `AUTO_PRERELEASE` 设为 `true`（默认未设置）。
- 稳定版由人把预发布发布为正式版，`sign-and-repackage.yml` 在 `release: published` 时签名，规则不变。

## 崩溃符号

MinGW 构建不产生 PDB。Windows 构建加了 `-g1` 和 `-Wl,--build-id`（`cmake/compile_definitions/windows.cmake`），每个模块带一个 CodeView GUID，
strip 和签名都不会改变它，崩溃转储（minidump）靠它找到符号。

- 打包时 `scripts/ci/make-symbols.py` 用 `dump_syms`（固定版本和 SHA-256）读未 strip 的模块里的 DWARF，生成 Breakpad 符号，
  存成 `<模块名>/<调试 ID>/<模块名>.sym`，连同 `symbols-manifest.json` 打成 `Sunshine.<版本>.Symbols-x64.zip`，作为发布资产和工件上传；
  随后把安装包和便携包里仍带 DWARF 的模块 `strip` 掉，所以安装包不带 DWARF：没有符号的模块也会被 strip，并记进 `symbols-manifest.json` 的 `stripped_without_symbols`；带数字签名的模块不动（strip 会破坏签名），记进 `kept_signed_with_debug_info`，打包日志里有一条警告。安装器外壳在链接时就去掉调试信息。
- `sunshine.exe` 或 `sunshinesvc.exe` 没有符号时打包失败。
- 解析崩溃：`minidump-stackwalk --symbols-path <解压目录> x.dmp`。
- 符号文件格式与目录布局见 Breakpad 文档，`--build-id` 见 GNU ld 手册，二者登记在 `docs/authority/source-index.tsv`（`breakpad-symbol-files-20261115`、`gnu-ld-options-20261115`）。

## 在本地复现

- 分类：`printf 'README.md\n' | python3 scripts/ci/classify-changes.py --event push`
- 脚本单测：`python3 -m unittest discover -s scripts/ci -p 'test_*.py'`
- 工具链巡检（模拟告警）：`python3 scripts/ci/msys2-pins.py check --simulate openssl=4.2.0-1 --json`
- 工作流静态检查：`actionlint`（固定版本见 `workflow-lint` 作业）和 `python3 scripts/ci/check-workflow-shell.py`
- 链接检查：`python3 scripts/ci/check-doc-links.py --root .`
- 子模块：`scripts/ci/init-submodules.sh windows`（或 `windows-package`），只拉 Windows 构建读取的子模块。
- Windows 构建的命令在 `main.yml` 的 *Configure Windows* 和 *Build Windows* 步骤里，环境是 MSYS2 UCRT64。
- 在自己的 fork 里验证：fork 里 push 到 master 以外的分支不会触发；要么临时把分支加进 `on.push.branches`，要么用 Run workflow。

## 预期耗时

测量日期 2026-11-15，GitHub 托管的 `windows-latest`（4 vCPU）和 `ubuntu-24.04`。同一配置在不同虚拟机上的耗时可差 ±20–50%，
所以每次运行都把 runner 的 CPU 型号写进作业摘要。下表是从触发到 `CI gate` 通过的墙钟。

| 场景 | 墙钟 |
|---|---|
| 只改文档（链接检查） | 约 20–25 秒 |
| 只改 README | 约 35 秒 |
| 只改工作流 | 约 25 秒 |
| 只改 Web UI | 约 90 秒（4 个分片并行） |
| 改 C++ 的 PR，编译缓存命中 | 约 8.5 分钟：Windows 作业 7.7 分钟，其中子模块 38 秒、工具链约 2 分钟、configure 1 分钟、编译 2 分钟（98.8% 命中）、测试 25 秒 |
| master 上的代码 push，缓存命中 | 编译步骤 155 秒（98.3% 命中），再加打包 |
| master 上只改文档的 push | 约 35 秒，不构建、不发布 |
| 冷缓存的完整构建（夜间、缓存被清空后） | Windows 作业 48–66 分钟（含打包），其中编译 37–52 分钟 |

缓存命中率取决于编译缓存是在同样的 MSYS2 位置、同样的工具链版本上保存的；工具链包更新后的第一次构建会部分未命中。

## 来源

文中引用的外部文档记录在 `docs/authority/source-index.tsv`，键名带日期，例如
`github-docs-required-checks-20261115`、`github-rest-compare-20261115`、`ccache-manual-20261115`。