# asterion-factor

独立的单合约成交动量因子计算与评价程序，装配工具插件，使用类型化 Protobuf 输入，输出逐事件因子、未来收益标签、Pearson 和 Spearman 相关性，并持久化实验输入与结果。

```sh
cmake --build build/Debug --target asterion-factor
asterion-factor --input /absolute/input.pb --directory /absolute/empty-result-directory
```

不拥有订单执行链，不把样本内因子评价等同于交易回测。当前最多 10000 笔成交，至少 30 个有效评价样本；窗口按事件数计算，常量序列相关性为未定义。

已接入 Task Service / Agent / Terminal，支持按类型领取任务、进度、取消、显式重试与持久化结果。安装包包含本机 Factor 及远程 Linux x86_64 Factor。具体公式、输入边界、持久化及测试见 [因子研究](../../../docs/factor-research.md)。
