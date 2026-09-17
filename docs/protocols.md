# 当前接口与持久化协议 v1

此文件描述已实现的限量 CSV 切片；目标通用协议见 architecture.md。

## 任务

`POST /api/v1/imports` 接受 command_id/source/csv，成功返回 202 和 job_id。相同 command_id、相同输入返回原任务，不同输入返回 409。输入结构错误返回 422；内容质量由 worker 校验，失败进入 FAILED。空来源响应不能发布为空快照。

状态：QUEUED → RUNNING → SUCCEEDED / FAILED / CANCELLED。RUNNING 租约过期可以被重新领取，attempt 增加并更换随机 fencing token。PostgreSQL 行锁和 SKIP LOCKED 防止并发领取。续期、失败及发布都要求匹配当前 token、RUNNING 状态与未过期租约。取消是逻辑取消，当前有界子进程可以完成计算，但不能再发布。

失联不会立即失败任务，过期后重试。worker 每约 lease_seconds/3 续期。单 worker 一次运行一个任务；当前没有内核级 CPU/内存限制，只有限量输入与隔离子进程。

## 发布

worker 上传至对应 job 的 publish 入口，携带 X-Lease-Token。API 锁定任务，验证租约及内容，对本次有界 CSV 重新校验并与标准化字节比较；写入唯一文件，fsync 文件及目录，确认 hash，再次检查租约后在同一 PostgreSQL 事务内登记快照并完成任务。

此版本在 API 线程池中重新校验最多 2 MB CSV，属于开发切片限制。通用版本应使用持久化验证证据和 worker 暂存/manifest 提交，避免在 serve 重做大量计算。

相同 token 与相同结果重复发布返回原快照；不同内容返回 409。失败事务可能留下无目录引用文件，但列表和查询只读取已登记快照。不使用文件存在作为发布成功依据。当前尚无孤儿回收任务。

数据文件使用服务端生成的 UUID 命名；客户端提交的 ID 必须先命中已发布目录，不能提供任意文件路径。快照返回逻辑 URI 和内容 SHA-256，跨机存储映射未实现。

## 安全与范围

仅监听 127.0.0.1；所有 API 需要 Bearer token。令牌来自服务端环境，前端仅保存在内存中。当前是本机单操作者开发认证，未区分 worker 与 UI 身份，不能作为远程多租户部署方案。Tauri 不授予外部页面原生权限，也没有 shell/文件/凭据操作权限。

任务列表最多返回最近 100 项，快照最多 100 项；行情目前为前 1,000 行。后续需加入完整分页与服务端范围查询。普通 API 类型由 OpenAPI 生成；WS 事件协议尚未实现。
