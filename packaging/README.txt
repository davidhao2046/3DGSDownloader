3DGSDownloader {{VERSION}} — Windows x64
made with love by Haodawei.

使用
1. 将整个 ZIP 解压到一个独立文件夹，保留 exe、DLL 和 licenses 文件夹。
2. 运行 3DGSDownloader.exe，粘贴影石 3D 时光舱公开分享链接。
3. 选择下载目录和资源，开始下载；完成后可在任务行打开结果文件夹。

运行要求
- Windows 10 / 11 x64。
- 支持 Vulkan 1.1、VK_EXT_surface_maintenance1、VK_EXT_swapchain_maintenance1
  等程序所需扩展的显卡驱动。请从显卡厂商获取驱动。
- Microsoft Visual C++ v14 x64 运行库，版本不得低于构建工具链 {{MSVC_TOOLSET_VERSION}}。
  微软官方最新 x64 安装包：https://aka.ms/vc14/vc_redist.x64.exe
  官方说明：https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist
  安装依赖后再运行。

任务与文件
任务记录和偏好保存于 %LOCALAPPDATA%\3DGSDownloader\tasks.json，下载文件保存到
用户选择的位置；解压目录不包含用户数据。每条任务生成独立子文件夹，已有文件不覆盖。
重启不会自动联网下载。取消保留已完成文件；重试未完成文件时从头下载，不支持断点续传。
下载的是来源原文件；不提供格式转换、3D 预览或视频视角调整。

命令行
3DGSDownloaderCli.exe --help 显示命令语法，与 GUI 共用下载和计划服务。
使用说明：https://github.com/davidhao2046/3DGSDownloader/blob/{{SOURCE_COMMIT}}/docs/usage.md

许可与来源
本程序自有代码使用 MIT 许可证，见 LICENSE。
Skia 使用 BSD 3-Clause，SkiaSharp 使用 MIT，完整原生依赖声明见：
  licenses/Skia-LICENSE.txt
  licenses/SkiaSharp-LICENSE.txt
  licenses/SkiaSharp-THIRD-PARTY-NOTICES.txt
Vulkan-Headers 保留 Apache-2.0 / MIT 声明，见 licenses 中对应文件。
系统字体、Windows 组件和 Vulkan 驱动由运行环境提供，不随本包分发。
下载内容的版权由其权利人保留，不属于本程序的 MIT 授权范围。
3DGSDownloader 是独立项目，与 Insta360 无官方关联。

来源提交：{{SOURCE_COMMIT}}
manifest.json 记录本包文件大小及 SHA-256；发布页的 SHA256SUMS 用于核对完整 ZIP。
项目：https://github.com/davidhao2046/3DGSDownloader
