import { Connection } from "@asterion/workbench/components/Connection";
import { nativeDesktop } from "@asterion/desktop-bridge/desktop";
import { BackupSettings } from "./BackupSettings";

import type { UiModule } from "@asterion/workbench/extensions/modules";
import type { PanelContext } from "./context";
import type { SettingsContext } from "./context";

function View({
  connection,
  busy,
  toggleService,
}: SettingsContext["services"]) {
  return (
    <>
      <h2 className="settings-section-title">运行环境</h2>
      <div className="setting-row">
        <div>
          后台连接
          <small>
            {connection.starting
              ? "正在读取状态"
              : connection.connected
                ? "本机服务可用"
                : "本机服务未连接"}
          </small>
        </div>
        <span className={connection.connected ? "good" : "muted"}>
          ● {connection.connected ? "运行中" : "未连接"}
        </span>
      </div>
      <div className="setting-row directory-row">
        <div>
          数据目录<small>{connection.directory || "尚未初始化"}</small>
        </div>
      </div>
      <p className="settings-note">
        关闭工作台窗口后，后台任务继续运行。停止服务会中断正在执行的任务，数据仍保留。
      </p>
      {nativeDesktop ? (
        <button
          className="service-action"
          disabled={busy || connection.starting}
          onClick={toggleService}
        >
          {busy
            ? "正在处理…"
            : connection.connected
              ? "停止后台服务"
              : "启动后台服务"}
        </button>
      ) : (
        <Connection onConnect={connection.setToken} />
      )}
    </>
  );
}
export const uiModule = { 
  id: "asterion.ui.maintenance-panel",
  settings: [
    {
      id: "terminal.services",
      scope: "services",
      title: "本机服务",
      order: 3,
      render: (context) => <View {...context} />,
    },
    {
      id: "terminal.backup",
      scope: "empty",
      title: "备份与恢复",
      order: 4,
      render: () => <BackupSettings />,
    },
    {
      id: "terminal.about",
      scope: "empty",
      title: "关于",
      order: 5,
      render: () => (
        <>
          <div className="about-mark">✧</div>
          <h2>Asterion Terminal</h2>
          <p>星枢 · 期货研究与交易工作台</p>
          <small>版本 0.1.0 · 数据功能基础版</small>
        </>
      ),
    },
  ],
} satisfies UiModule<PanelContext, SettingsContext>;
