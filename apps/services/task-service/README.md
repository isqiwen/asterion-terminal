# asterion_task_service

当前持久化下载、回测和因子任务：幂等提交、执行尝试、进度、取消、显式重试及结果确认。
服务按实例配置在内部计算池执行，不启动独立worker，也不请求Agent调度任务。
历史版本和下载发布仍由配对的`asterion_data_service`拥有；Data／Research职责拆分另见
[目标设计](../../../docs/reviews/service-owned-tasks-design.md)。

状态与I/O线程拥有任务状态；计算池独占算法输入，通过原子进度和future交回结果。
一条日志线程及有限文件池处理持久化，执行线程不直接修改任务账本。回测使用任务捕获的风险插件；
下载从Data获取本次授权凭据，只在内存执行期间使用，不写入任务定义、日志或结果。

取消协作停止，确认结果后才释放并发名额；重启中断的任务需显式重试。
升级先冻结队列并等待执行和发布结束，再停止服务；普通停止沿用RpcHost有限排空期限。

启动只读`--config <node>/config/services/<实例>/task.json`，`task_concurrency`默认1、允许1–2。
配置由部署复制，启动不生成、不转换现有文件。没有worker端点或Agent任务控制连接。
详见[任务管理](../../../docs/tasks.md)。
