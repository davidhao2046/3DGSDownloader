# 网络与下载边界

本模块使用 Windows SDK WinHTTP/BCrypt，不引入额外下载库。GUI 和 CLI 经 `core/application/transfer_service.*` 调用；同步 WinHTTP 仅运行于下载 worker，UI 不读网络或等待句柄。

- `http.*`：HTTPS 443、明确页面与资源域名、逐跳重定向校验（最多 4 次），禁用 Cookie/认证/自动重定向；不记录请求 URL。自动代理遵循 Windows WinHTTP 配置，不借用浏览器登录。
- `bounded_json.hpp`：严格 UTF-8、有界深度/节点/字符串、拒绝重复键；用于解析来源页面和资源元数据。
- `adapters/sources/insta360.*`：当前公开页面结构、任务编号/公开/就绪校验，资源 path+MD5 身份，文件名百分号解码与 Windows 路径校验；原格式 SOG/JSON/ZIP/MP4。未知结构/格式/域名明确报错。
- `core/application/download.cpp`：同目录 CREATE_NEW 随机 `.part`，64 KiB 流式写入和 MD5/SHA-256，检查声明长度与 MD5，flush 后无覆盖发布。失败/取消清理本次临时文件，保留完成文件。目录句柄固定边界；文件名来自已验证计划。
- `transfer_service.*`：单任务、带快照的完成消息、终态唯一、CAS 取消/发布门、独立终态收据。没有跨进程在途恢复或断点追加。

超时设为解析/连接/发送各 5 秒、接收 10 秒；每个有限调用前后和每块读取检查取消，取消不是立即中断当前 WinHTTP 调用。解析整体检查 90 秒上限（仍可能多出正在执行的一次调用）；单次响应读取检查 1 小时上限。429、5xx 和列出的暂时传输错误最多 3 次，从头重试，退避 0.5/1 秒。401/403 触发有限的公开页面重新解析；资源身份变化则拒绝旧选择。未承诺遵循所有服务端限流策略或代理环境兼容。

元数据探测只读响应头；206 校验 `bytes 0-0/总量`，忽略 Range 的 200 使用 Content-Length，无长度保留未知。完整 GET 必须 200，不把探测前缀追加到文件。WinHTTP 返回 EOF 不一定意味着完整，额外核对 Content-Length，断流测试覆盖此点。

内部 `--transfer-chunk-delay-ms` 参数只为真实 UI 验证增加 worker 延迟，默认零，上限每块 2000 ms；延迟段每 20 ms 检查取消。禁止据此报告实际下载速度或输入延迟指标。回环 HTTP 仅测试用显式 Route，产品来源不允许输入任意下载 URL。

Windows API 依据：[超时](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpsettimeouts)、[响应头](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpqueryheaders)、[句柄生命周期](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpclosehandle)。自动化覆盖见 [下载测试](../../tests/transfer_tests.cpp)；构建与验证方法见 [构建说明](../../docs/building.md)。
