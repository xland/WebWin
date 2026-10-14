# CI/CD 与发布需求（草稿）

> 状态：需求记录，尚未实现。后续方案确定后补充具体实现细节。

## 背景

本项目托管在 GitHub，希望借助 GitHub Actions 的自动编译能力，免去本地手工出包流程。

## 需求

1. **代码托管**
   - 项目源码托管到 GitHub 仓库。

2. **自动编译（GitHub Actions）**
   - 推送 / PR 时能自动编译，验证代码可构建。
   - 编译目标：`WebWin.vcxproj`，平台 `x64`（项目已移除 Win32 配置）。
   - 需覆盖 `Debug` 与 `Release` 两种配置。

3. **Release 自动发布**
   - 在 GitHub 上创建 Release（打 Tag / 发布版本）时，自动触发编译。
   - 编译产物自动上传并附加到该 Release 中，供直接下载。

## Runner 与工具链选型（调研结论 · 2026-10）

结论：**不需要在 CI 里自行安装 VS 2026**，GitHub 已提供预装 VS 2026 的托管镜像。

- GitHub 托管 Runner 标签 `windows-2025-vs2026`（Windows Server 2025 + Visual Studio 2026）可用，
  工作流里写 `runs-on: windows-2025-vs2026` 即可，无需任何安装步骤。
- VS 2026 = 版本 18.x，MSVC 工具集即 `v145`，与本项目 `<PlatformToolset>v145</PlatformToolset>` 匹配。
- 该 issue（actions/runner-images#13291）已于 2026-06 关闭完成；Azure DevOps 侧同期提供同名镜像。
- **限制**：GitHub 官方文档的标准 Runner 表格中，`windows-2025-vs2026` 只列在
  *public repositories* 一栏，*private repositories* 一栏没有该标签；
  且 docs 声明 Beta 镜像不在 SLA 保障内、可用性可能按 public/private 区分。
  → 私有仓库能否使用该标签，需要实跑一次验证。
- ARM64 侧另有 `windows-11-vs2026-arm`，本项目暂不涉及。

### 备选方案（若私有仓库不可用 `windows-2025-vs2026`）

| 方案 | 说明 | 评价 |
| --- | --- | --- |
| A. 用 `windows-2025-vs2026` | 镜像自带 VS 2026 / v145 | **首选**，零安装成本 |
| B. CI 内自行安装 VS 2026 | 下载 `vs_BuildTools.exe` 并 `--add Microsoft.VisualStudio.Workload.VCTools` | 技术上可行，但每次流水线都要联网下载安装，耗时长（数 GB、十余分钟量级），拉长发布流程，不推荐作为常规方案 |
| C. 工具集降到 `v143`（VS 2022） | `windows-latest` / `windows-2022` / `windows-2025` 均预装 VS 2022 | 最稳，代价是本地 VS 18 与 CI 编译器版本不一致 |
| D. 自建 Runner（self-hosted） | 本机已装 VS 18 Community | 完全可控，但需长期维护机器在线 |

### 建议

先按方案 A 写工作流；若私有仓库不支持该标签，则退回方案 C（工程改用 v143）或方案 D。
方案 B 仅作为兜底，不建议常态化使用。

## 待确认事项

- 本仓库是 public 还是 private（决定能否用 `windows-2025-vs2026`）。
- Release 产物清单：exe 之外是否附带 pdb、符号包、依赖说明等。
- 是否提供安装包（ZIP 便携版 vs 安装程序）。
- Tag 命名规范与版本号来源（`main.cpp` / rc 文件 / Actions 变量）。
- 是否需要代码签名。
- 编译触发时机细节：仅 Tag 推送触发，还是 Release 发布事件触发。

## 项目构建相关现状

| 项目 | 值 |
| --- | --- |
| 解决方案 | `WebWin.slnx` |
| 工程文件 | `WebWin.vcxproj` |
| 平台 | 仅 `x64` |
| 工具集 | `v145` |
| 语言标准 | C++20（`/utf-8`） |
| 运行库 | Debug `MultiThreadedDebug` / Release `MultiThreaded`（静态链接） |
| 子系统 | Windows（GUI） |
| Debug 产物 | `build\debug\WebWin.exe` |
| Release 产物 | `build\release\WebWin.exe` |
| 中间文件 | `build\temp\debug\`、`build\temp\release\` |

## 本地编译命令

```
msbuild WebWin.vcxproj /p:Configuration=Release /p:Platform=x64
```
