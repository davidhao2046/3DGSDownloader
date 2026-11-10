# Windows 发布

发布包以 Windows x64 ZIP 提供，包含桌面程序、CLI、锁定的 Skia DLL、运行说明、源码版本与文件哈希清单，以及 MIT 和第三方许可证。用户任务、下载数据、测试程序、调试符号和构建日志不进入包。

## 本地验证与打包

在已安装所需工具链与 Vulkan 驱动的 Windows 设备上运行：

```powershell
.\third_party\prepare.ps1
.\tools\build.ps1 -Configuration Release -Verify
.\tools\package.ps1 -Version 0.1.0-beta.1 -BuildDirectory build/windows-release
```

正式打包要求 Git 工作树干净。脚本核对 Release 构建收据、源码及产物哈希，并检查 x64 格式、依赖锁和包内白名单；展开后校验文件并运行 CLI 帮助。输出位于 `artifacts/releases/<版本>/`，包含 ZIP 与 `SHA256SUMS`，不会覆盖已有版本目录。

## GitHub Actions

推送 `v<版本>` 标签，或在 Actions 的 **Windows release → Run workflow** 中输入版本号，可从干净 Windows runner 获取锁定依赖、构建、测试并打包。第三方 Actions 固定到提交 SHA。

构建任务只有仓库读取权限，checkout 不保留凭据；独立发布任务拥有创建 Release 所需的 contents 写权限。成功后生成 **Draft / Pre-release**，附 ZIP 和 SHA-256 清单。版本已存在时不覆盖资产。已有标签必须指向本次构建提交（支持带注释标签）；手动运行时若标签不存在，则为该提交创建标签。标签指向不一致时停止发布，不移动旧标签。

托管 runner 缺少所需 Vulkan 呈现设备，使用与 PR 相同的 `tools/test-hosted.ps1`，只排除 `graphics.skia_vulkan_worker`。其余当前 10 项领域、下载、存储、CLI、文本和栅格矩阵测试必须实际执行并通过；脚本核对注册清单与结果，拒绝漏测或跳过。不能将这一步描述为完整 GPU 或真实 UI 验收。

发布草稿前，应下载本次 CI 生成的 ZIP，校验 SHA-256，并在支持的 Windows 设备上检查实际窗口呈现、新建弹层、中文文字、关闭及重开。涉及下载或偏好行为的变更，还需验证对应真实流程。测试记录与发布包必须对应同一提交。

## 运行要求

需要 Windows x64、支持所需 Vulkan 扩展的显卡驱动，以及不低于构建工具链版本的 Microsoft Visual C++ v14 x64 运行库。运行库下载使用 [Microsoft 官方入口](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)。

当前预览包没有代码签名；它不包含安装器、自动更新器或跨进程断点续传功能。仓库仍为私有时，Release 只对有仓库访问权限的用户可见。
