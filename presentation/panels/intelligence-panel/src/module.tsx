import { widgets } from "./widgets";
import type { UiModule } from "@asterion/workbench/extensions/modules";
import type { PanelContext } from "./context";
import type { SettingsContext } from "./context";
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
export const uiModule = { 
  extensions: widgets,
  id: "asterion.ui.intelligence-panel",
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
} satisfies UiModule<PanelContext, SettingsContext>;
