# Node Manager

`asterion_node_manager` 是 Terminal 所在机器上的独立节点管理进程，拥有 Agent 登记、管理连接、
节点状态汇总和维护协调。Agent 负责本机业务进程监督，业务请求直接连接所属服务。

完整设计见 [Node Manager 架构设计](../../../docs/architecture/node-manager.md)，
共同约束见[总体架构](../../../docs/architecture.md)。

源码入口：

- [main.cpp](main.cpp)：进程启动、互斥锁、日志和 RPC 接入。
- [manager.cpp](manager.cpp)：节点表、登记恢复、请求分派和管理操作门控。
- [agent_client.cpp](agent_client.cpp)：Agent 连接、状态观察、服务部署及控制。
- [node_enrollment.cpp](node_enrollment.cpp)：SSH 登记、身份替换和远程维护。
- [CMakeLists.txt](CMakeLists.txt)：本模块的程序、库目标与依赖。
