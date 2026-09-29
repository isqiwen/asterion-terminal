# Terminal 中英文

国际化属于 Terminal 内置宿主基础设施，不新增功能插件类型，也不进入 C++ Core。当前提供简体中文（`zh-CN`）和英文（`en-US`），默认中文。首次启动页及“设置 → 外观 → 语言”均可切换，即时更新界面，并通过本机 `asterion.locale` 偏好保存。语言与密度、涨跌配色独立。

## 归属与契约

- `apps/clients/terminal/src/i18n/`：语言状态、订阅、偏好持久化、资源校验、参数插值和错误呈现。
- `apps/clients/terminal/src/i18n/locales/`：宿主窗口、导航容器、设置、启动流程的中英文资源，命名空间为 `host`。
- `apps/clients/terminal/plugins/<plugin>/locales/`：插件自己的导航名称、面板、总览卡片及交互文案。
- 插件通过 `TerminalPlugin.languageResources` 贡献两种语言的资源，注册时以插件 ID 作为命名空间。插件通过公开契约导出的 `translate`、`getLocale` 等能力使用资源。

当前采用中文源文案作为消息键；键只用于本地资源查找，不用于协议、插件身份或持久化业务配置。带动态值的句子使用完整消息和 `{name}` 占位符，禁止通过翻译用户输入构造消息键。注册拒绝重复命名空间、缺失语言键、空文案和占位符不一致；缺失消息或参数显式报错，不能静默返回键名。

插件工作区标题使用 getter 在渲染时获取当前翻译，面板及卡片在渲染时调用翻译函数，不能在模块加载时冻结译文。工作区、标签和设置页面使用稳定 ID，语言切换不改变它们的身份。

## 数据与错误边界

日期和时间按当前语言格式化；历史期货成交时间仍明确使用北京时间，不因为语言切换改变交易时区。金额、合约代码、路径、账户和连接参数保持原值，不翻译协议字段和技术标识。

C++ 只输出英文诊断，不含界面语言文字。错误码定义于 `core/include/asterion/foundation/error.hpp`（invalid_request、unavailable、conflict、permission_denied、resource_exhausted、cancelled、not_found、recovery_required、operation_failed、internal_error），`classify` 把异常统一映射为错误码；交易、策略、行情、任务服务与 Node Agent 的 Protobuf 错误都携带错误码，Terminal 客户端用 `throw_remote_error` 还原，C ABI 原样传给界面，不再在跨进程时丢失。

界面摘要按以下顺序确定：已登记的英文诊断（`apps/clients/terminal/src/i18n/locales/diagnostics.*.json`，键为中文文案）显示为当前语言的具体原因；否则按错误码显示通用摘要。“详情”始终展开 `code: 原始诊断`。新增面向用户的 C++ 诊断时应同时登记到 diagnostics 语言包；未登记的诊断仍可用但只显示通用摘要。服务日志、操作系统错误和第三方诊断不宣称已全部翻译。

资源随安装包提供，无需联网获取语言包；当前不提供用户安装语言包、动态第三方语言扩展或系统语言自动推断。
