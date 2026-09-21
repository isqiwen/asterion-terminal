import { savePreferences } from "./preferences";

import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";

function View({ preferences }: SettingsContext["appearance"]) {
  return (
    <>
      <h2 className="settings-section-title">界面</h2>
      <div className="setting-row">
        <div>
          <label htmlFor="density">信息密度</label>
          <small>调整表格行高与控件间距</small>
        </div>
        <select
          id="density"
          value={preferences.density}
          onChange={(e) =>
            savePreferences({
              ...preferences,
              density: e.target.value as "compact" | "comfortable",
            })
          }
        >
          <option value="compact">紧凑</option>
          <option value="comfortable">舒适</option>
        </select>
      </div>
      <div className="setting-row">
        <div>
          <label htmlFor="colors">涨跌颜色</label>
          <small>应用于行情图表与实时报价</small>
        </div>
        <select
          id="colors"
          value={preferences.colors}
          onChange={(e) =>
            savePreferences({
              ...preferences,
              colors: e.target.value as "china" | "international",
            })
          }
        >
          <option value="china">红涨绿跌</option>
          <option value="international">绿涨红跌</option>
        </select>
      </div>
      <p className="settings-note">修改即时生效，工作区与已选对象保持不变。</p>
    </>
  );
}
export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  id: "asterion.appearance",
  requires: [],
  settings: [
    {
      id: "terminal.appearance",
      scope: "appearance",
      title: "外观",
      order: 0,
      render: (context) => <View {...context} />,
    },
  ],
};
