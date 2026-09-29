# 客户端直接启动

Terminal 已移除平台账户注册、登录、退出、会话轮询和账户权益界面。客户端启动不查询 Cloud，不要求 Cloud 地址、身份 Cookie 或独立认证工程。Terminal 保留服务初始化流程，数据服务尚未接入时继续明确显示不可用，不伪造数据。

Electron 已删除账户认证 IPC 与平台会话检查，仍严格校验发送窗口和页面来源、限制请求体和并发、隔离渲染进程并禁用 Node 集成。Vite C++ 开发桥仍仅监听回环地址并校验 Host、Origin、方法和内容类型；该开发桥不是生产 Web API。远程服务的 mTLS、SSH 校验、行情商登录和交易风控不属于被移除的平台账户系统。

测试和 CI 不再启动、检出或访问 Cloud：`pnpm test:e2e`、`pnpm test:desktop` 可以独立运行。本仓库不修改或删除同级 `asterion-cloud` 工程、数据库或既有用户数据；旧客户端存储也不作自动清理。

移除账户门禁不等于完成公共多用户服务授权。Web/Mobile 数据服务的上线边界仍见 `client-ui.md`，本次未改变对外部署范围。

Web/Mobile 原型及其测试和构建入口随后按维护者要求删除；当前只交付 macOS Terminal。
