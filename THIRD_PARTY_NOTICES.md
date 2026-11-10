# 第三方组件与来源

3DGSDownloader 自有代码使用根目录的 [MIT 许可证](LICENSE)。第三方文件保留各自的版权声明和许可证，不因根目录许可证而改变授权。

| 组件 | 仓库内内容 | 许可证与来源 |
|---|---|---|
| Skia 原生 C 接口 | `third_party/skia/include/c/` 头文件 | [BSD 3-Clause 原文](third_party/skia/LICENSE)；[版本、上游地址与文件哈希](third_party/skia/source-lock.json) |
| SkiaSharp.NativeAssets.Win32 2.88.8 | 发布包中的 Windows x64 `libSkiaSharp.dll` | 官方 NuGet 包的 [MIT 许可证](third_party/skia/LICENSE-SkiaSharp.txt) 与 [完整第三方声明](third_party/skia/THIRD-PARTY-NOTICES.txt) |
| Vulkan-Headers | `third_party/vulkan/include/` 头文件 | [上游许可说明](third_party/vulkan/LICENSE.md)，具体文件以 SPDX 标记为准；保留 [Apache-2.0](third_party/vulkan/LICENSES/Apache-2.0.txt) 和 [MIT](third_party/vulkan/LICENSES/MIT.txt)；[版本及哈希](third_party/vulkan/source-lock.json) |

`libSkiaSharp.dll` 从官方 NuGet 包获取；源码仓库不提交该二进制，发布 ZIP 随附该文件及上述许可。来源、版本及包/DLL/许可文件校验值见 [依赖锁](third_party/dependencies.json)。当前使用上游预编译二进制，尚未验证从完整上游源码可重现生成同一 DLL。构建要求见 [构建说明](docs/building.md)。

绘制与呈现代码的适配来源及原文件哈希保留在 [graphics/provenance.json](graphics/provenance.json)。该记录用于追踪源码来源，不包含开发机路径。

字体由 Windows 的 DirectWrite 系统字体集合提供，仓库不打包字体、原软件图标或安装目录资源。Windows 系统库和 Vulkan 驱动由运行环境提供。

程序依赖 Microsoft Visual C++ v14 x64 运行库，由用户通过 [Microsoft 官方下载页面](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) 安装；发布包不从开发机复制 Visual Studio 运行库。

影石分享内容不属于本仓库的授权范围；本程序的 MIT 许可证不授予下载内容的版权。3DGSDownloader 是独立项目，与 Insta360 无官方关联。
