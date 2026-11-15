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

- 保存：只有 master 的 push、手动运行和夜间构建保存（`ccache-windows-<run id>`，只保留最新一份，旧的由修剪步骤删除）。
  在自己 fork 里运行这个工作流时，fork 里的 pull request 也保存，因为 fork 没有上游 master 的缓存可恢复。
- 恢复：除夜间构建外，所有运行按前缀 `ccache-windows-` 恢复最近的一份；夜间构建不恢复，从冷缓存开始编译。
  pull request 能读到 master 的缓存
  （缓存按分支隔离，pull request 可读基础分支，源 `github-docs-dependency-caching-20261115`）。
- 命中的前提是编译器路径和系统头文件路径一致：MSYS2 由 setup-msys2 装在 runner 的临时目录（`D:\a\_temp\msys64`），不要改用镜像自带的
  `C:\msys64`。位置一变，所有预处理结果和缓存键都不同，整份缓存失效（实测同一份缓存：临时目录 98% 命中，`C:\msys64` 17%）。
- 构建标识（提交、分支、渠道、CI 运行）不进入大多数编译单元：版本号只由 `src/build_info.cpp` 读取，
  新提交只会重编译这一个文件和真正改动的文件。

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
  随后把安装包和便携包里的模块 `strip` 掉，所以安装包不带 DWARF。
- `sunshine.exe` 或 `sunshinesvc.exe` 没有符号时打包失败。
- 解析崩溃：`minidump-stackwalk --symbols-path <解压目录> x.dmp`。

## 在本地复现

- 分类：`printf 'README.md\n' | python3 scripts/ci/classify-changes.py --event push`
- 脚本单测：`python3 -m unittest discover -s scripts/ci -p 'test_*.py'`
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
