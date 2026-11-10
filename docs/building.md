# 构建说明

## 前提

- Windows x64。
- Visual Studio 的 C++ 桌面开发工具链，以及 CMake/Ninja 组件；CMake 至少 3.25。
- PowerShell 7，`pwsh` 在 PATH 中，用于 CLI 集成测试。
- 运行桌面程序和图形测试时，需要支持 Vulkan 1.1、`VK_EXT_surface_maintenance1` 和 `VK_EXT_swapchain_maintenance1` 等所需扩展的驱动。
- 与 [依赖锁](../third_party/dependencies.json) 匹配的原生 `libSkiaSharp.dll`。

仓库只包含第三方头文件、许可证和锁定信息；依赖准备脚本从官方 NuGet 获取 SkiaSharp.NativeAssets.Win32 2.88.8 中的 Windows x64 DLL，并校验下载包和文件哈希。这里使用上游预编译二进制，不声称已实现 Skia 的可重现源码构建。不能用其他同名 DLL 绕过哈希检查。

## 准备依赖

在仓库根目录打开 PowerShell：

```powershell
.\third_party\prepare.ps1
```

脚本校验 SHA-256 后准备 `third_party/installed/`。该目录和下载缓存均已被 Git 排除；脚本不会搜索其他软件的安装目录。离线准备可以显式传入 `-SkiaSource`，仍要求符合当前锁。第三方许可证见 [第三方声明](../THIRD_PARTY_NOTICES.md)。

## 编译与测试

```powershell
.\tools\build.ps1 -Configuration Release -Verify
& '.\build\windows-release\bin\3DGSDownloader.exe'
```

脚本通过 `vswhere` 自动发现带 MSVC x64 和 C++ CMake 工具组件的最新 Visual Studio。需要选择特定安装时可显式指定：

```powershell
$VisualStudioPath = Read-Host '请输入 Visual Studio 安装根目录'
.\tools\build.ps1 -Configuration Debug -VisualStudioRoot $VisualStudioPath -Verify
```

常用选项：

| 选项 | 用途 |
|---|---|
| `-Configuration Debug` / `Release` | 选择构建配置 |
| `-BuildDirectory build/custom` | 使用独立构建目录 |
| `-Verify` | 构建后运行 CTest |
| `-Fresh` | 重新生成 CMake 配置 |
| `-Clean` | 执行完整重编 |
| `-ConfigureOnly` | 只配置，不编译 |

默认产物位于 `build/windows-debug/bin/` 或 `build/windows-release/bin/`，包括桌面程序、CLI 和本地运行依赖。构建日志及输入/产物收据保存在 `artifacts/build/`，这些都是本机生成文件，不提交到仓库。

也可以在已配置 MSVC x64 环境的终端使用 CMake presets：

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

测试覆盖计划历史、存储、下载、队列、文本、CLI 和视觉矩阵。图形测试依赖实际驱动环境；测试通过不代表真实窗口、中文 IME、所有 DPI 或全部来源分享形式都已验证。

面向 `main` 的 PR、`main` 推送及手动触发会执行 GitHub Actions 的 `Windows x64 checks`：在 Windows 2022 上构建 Release，并运行无需 Vulkan 呈现设备的测试。检查只排除 `graphics.skia_vulkan_worker`；其余当前 10 项必须全部执行并通过，缺少注册、漏测、重复、禁用或跳过都会失败。JUnit 报告作为 Actions artifact 保留 14 天。

PR 与 Release 共用 `tools/test-hosted.ps1`。该脚本核对 CTest 注册清单和实际报告，新增测试默认纳入。本地 `-Verify` 仍运行完整 11 项，不受托管配置影响；托管检查不替代实际窗口、GPU、中文 IME 和焦点验证。ZIP 打包和 Release 步骤见 [发布说明](releasing.md)。

## 贡献边界

提交方式及验证要求见 [贡献指南](../CONTRIBUTING.md)。新增来源适配、下载逻辑或界面行为前，请阅读 [架构说明](architecture.md) 和根目录 [AGENTS.md](../AGENTS.md)。构建产物、真实下载文件、任务工作区、个人计划、凭据及本地验证数据不进入 Git。提交新依赖时同时提供来源、版本锁和许可证。
