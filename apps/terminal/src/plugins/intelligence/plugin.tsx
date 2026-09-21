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
  id: "asterion.intelligence",
  requires: ["asterion.overview"],
  workspaces: [
    {
      id: "workspace.intelligence",
      title: "情报",
      icon: "◎",
      sections: [{ title: "事件与报告", panel: "intelligence.reports" }],
    },
  ],
  panels: [
    {
      id: "intelligence.reports",
      scope: "empty",
      render: () => (
        <Unavailable
          title="事件与报告"
          status="未接入"
          detail="尚无情报数据"
          message="情报来源尚未接入。"
        />
      ),
    },
  ],
};
