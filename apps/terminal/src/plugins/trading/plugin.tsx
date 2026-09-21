import { widgets } from "./widgets";
import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";
function Unavailable({
  title,
  detail,
  message,
  status,
}: {
  title: string;
  detail: string;
  message: string;
  status: string;
}) {
  return (
    <>
      <div className="panel-heading">
        <h2>{title}</h2>
        <span className="panel-spacer" />
        <small>{status}</small>
      </div>
      <div className="table-empty-state">
        <span>{detail}</span>
        <p>{message}</p>
      </div>
    </>
  );
}
export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  extensions: widgets,
  id: "asterion.trading",
  requires: ["asterion.overview"],
  workspaces: [
    {
      id: "workspace.trading",
      title: "交易",
      icon: "⇄",
      sections: [{ title: "持仓与委托", panel: "trading.account" }],
    },
  ],
  panels: [
    {
      id: "trading.account",
      scope: "empty",
      render: () => (
        <Unavailable
          title="持仓与委托"
          status="账户 — · 环境：研究"
          detail="尚未连接交易账户"
          message="交易执行尚未开放，当前不能发送委托。"
        />
      ),
    },
  ],
};
