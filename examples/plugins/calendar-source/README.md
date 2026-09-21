# 日历数据源开发样例

复制本目录到独立仓库，修改插件 ID、版本、名称和实现后打包。它不生成行情或日历，不注册到正式发行版；真实数据由你配置的 HTTPS JSON 接口提供。

```sh
python -m asterion_plugin_sdk pack . ../calendar-source.zip
```

开发环境先安装项目源码中的 SDK（当前与终端源码同一 Python 包分发），或将 `src/asterion_plugin_sdk` 复制到 Python 搜索路径。样例仅导入标准库和公开 SDK，不导入终端内部模块。

在终端“设置 → 插件”选择 ZIP，核对内容后点击“信任并安装启用”；“数据源”配置 HTTPS 接口，再在数据同步选择自定义日历。“扩展 → 插件视图”显示该插件声明的接口信息。

日历接口接收 `exchange`、`start`、`end` 查询参数，返回数组，每行包含 `exchange`、ISO 日期 `date`、整数 `is_open`（0/1）、ISO 日期或 null 的 `previous_trading_day`。`probe=1` 用于连接检查。不要在 URL 中放置密钥。

本地代码以当前用户权限运行。包只支持纯 Python 源码和文本资源；额外纯 Python 依赖须一同打包，不执行 pip 或安装脚本。冻结版只承诺本样例及 SDK 用到的标准库集合，不支持任意原生扩展依赖。
