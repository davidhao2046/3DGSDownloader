# 计划存储与串行 I/O

本模块负责下载计划的持久化与串行文件操作。公共接口在 `plan_storage.hpp`；只依赖领域模型、C++20、Windows 文件 API 和系统 BCrypt 随机数。

`encode_plan` / `decode_plan` 使用 UTF-8 JSON、`schemaVersion=1`。只有 `DownloadPlan` 的持久字段进入文件：计划身份、规范分享链接和来源身份、资源元数据/输出名/勾选、输出目录、创建/修改时间、内部样本标记。会话、revision、历史、任务状态、草稿、资源签名 URL 不进入格式。每层对象字段必须完全匹配，拒绝重复键（含转义后的同名键）、未知字段、缺字段、类型转换、非法 UTF-8/代理对、未来版本与尾部内容；完整解码后调用 `validate_plan`。字节大小以 `uint64_t` 精确表示，不经 double。

格式读取上限为 8 MiB，解析深度 16、节点 120000、字符串 64 KiB、数组 10000 项；领域层资源上限更严格，目前为 4096。参数限制以领域校验为准。只接受格式所需的非负整数，不支持 JSON 注释、浮点字节大小或 BOM。未知版本/字段不会被丢弃后重新保存。

同步 API 供 CLI 和 I/O worker 使用，不得从 GUI 主线程调用。保存先验证和序列化完整快照，再在目标同目录用随机名 `CREATE_NEW` 创建自己拥有的临时文件，分块 `WriteFile`、`FlushFileBuffers`、关闭句柄，最后发布。默认 `create_new` 不覆盖目标；`replace_or_create` 只用于用户明确授权的目标。目标已存在时使用 `ReplaceFileW` 和唯一恢复副本；不存在时用不带替换标志的 `MoveFileExW`，目标竞态由失败解决，不改写竞争者的文件。

普通写入、刷新、只读属性、禁止删除共享句柄等失败保留旧文件并清理自己的临时文件，不扫描或删除别人的 `.tmp`。`ReplaceFileW` 的少数错误可能已经移动旧文件，因此传入恢复副本并尝试无覆盖回滚；若底层回滚也失败，错误 `recoveryPath` 指向仍保留的原文件。正式断电耐久性、文件系统损坏、网络盘和所有权限组合尚未验收，不能由本机测试推导。发布成功后清理恢复副本；清理因外部权限/占用失败时，已完成的保存不会被误报失败，旧副本可能保留。

上述边界依据 [Microsoft ReplaceFileW 返回值约定](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew) 与 [MoveFileExW 发布标志](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)。`FlushFileBuffers` 不等于已验证任意文件系统在断电后保留目录变更。

`IoWorker` 是拥有状态的单 FIFO。默认最多 8 个已受理但尚未取走结果的操作，避免通知接收方停止消费时无限增长。整个应用的计划发布共用一个 worker，所有目标及路径别名因此按受理顺序串行。SaveRequest 保留不可变 `SaveCapture`；ReadRequest 保留 `sessionId/requestId/capturedRevision`。结果拥有数据并原样带回身份，应用协调线程先核对身份，再调用 `replaceLoaded` 或 `acceptSaved`；保存 S 期间编辑成 T，S 成功只更新保存标记，T 仍 dirty。

通知回调运行在 I/O 线程，只应 `PostMessage`。`request_stop` 拒绝新请求并排空已经受理的操作，设置 stopped 后发最后通知。析构不 join；分离线程持有共享状态，回调接收者必须活到 stopped 通知。`WorkerOptions.testIoDelayMs` 和 `SaveFault` 仅用于内部验收：延迟发生在 worker，stop 会跳过剩余模拟延迟再完成队列；不声称可以强制中断真实卡住的 Windows I/O。

`storage.strict_atomic_async` 覆盖格式拒绝与中文/uint64 往返、无覆盖发布、三处写入失败注入、真实禁止替换句柄/只读文件保旧、外部临时文件不受影响、S/T 保存标记、同目标 FIFO、结果身份、队列上限及无 join 析构后的安全停止。它不是 GUI 打开/保存/关闭检查，真实窗口另由主工程验收。
