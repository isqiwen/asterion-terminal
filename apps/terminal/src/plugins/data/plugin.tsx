import { DatasetCatalog } from "./DatasetCatalog";
import { DataSources } from "./DataSources";
import { DataSync } from "./DataSync";
import { ReferenceCatalog } from "./ReferenceCatalog";
import { taskViews } from "./taskViews";

import type { TerminalPlugin } from "../../extensions/plugins";
import type { PanelContext } from "../workflow/context";
import type { SettingsContext } from "../workflow/settingsContext";
function Reference(context: PanelContext["data"], active: boolean) {
  const { openSync, connection, select, referenceRules, showReferenceRules } =
    context;
  return (
    <>
      <>
        <div className="panel-heading">
          <button
            className={!referenceRules ? "pressed" : ""}
            onClick={() => showReferenceRules(false)}
          >
            基础资料
          </button>
          <button
            className={referenceRules ? "pressed" : ""}
            onClick={() => showReferenceRules(true)}
          >
            合约目录
          </button>
          <span className="panel-spacer" />
          <small>合约信息与交易规则</small>
        </div>
        {referenceRules ? (
          <ReferenceCatalog
            api={connection.api}
            connected={connection.connected}
          />
        ) : (
          <DatasetCatalog
            key="contracts"
            api={connection.api}
            connected={connection.connected}
            onSnapshot={select}
            contractsOnly
            onSync={openSync}
          />
        )}
      </>
    </>
  );
}

function Sync(context: PanelContext["data"], active: boolean) {
  const { connection, prepared, openCatalog, submitted } = context;
  return (
    <DataSync
      onPrepared={prepared}
      api={connection.api}
      connected={connection.connected}
      onBrowse={openCatalog}
      onSubmitted={submitted}
    />
  );
}

function Catalog(context: PanelContext["data"], active: boolean) {
  const {
    connection,
    select,
    dataTarget,
    clearTarget,
    openResearch,
    openSync,
    refilled,
  } = context;
  return (
    <>
      {dataTarget && (
        <div className="panel-heading">
          <button onClick={openResearch}>返回研究（保留参数）</button>
          <button onClick={clearTarget}>清除定位</button>
        </div>
      )}
      <DatasetCatalog
        key={`datasets:${dataTarget?.versionId || ""}`}
        initialVersionId={dataTarget?.versionId}
        initialReportId={dataTarget?.reportId}
        onRefill={refilled}
        api={connection.api}
        connected={connection.connected}
        onSnapshot={select}
        onSync={openSync}
      />
    </>
  );
}

export const plugin: TerminalPlugin<PanelContext, SettingsContext> = {
  apiVersion: 1,
  id: "asterion.data",
  extensions: taskViews.map((value) => ({
    id: value.id,
    point: "tasks.views",
    value,
  })),
  settings: [
    {
      id: "data.sources",
      scope: "data",
      title: "数据源",
      order: 2,
      render: (context) => <DataSources api={context.api} />,
    },
  ],
  requires: ["asterion.overview", "asterion.identity", "asterion.tasks", "asterion.trading_time", "asterion.contract_roles"],
  workspaces: [
    {
      id: "workspace.data",
      title: "数据",
      icon: "▤",
      sections: [
        { title: "数据同步", panel: "data.sync" },
        { title: "数据集", panel: "data.catalog" },
        { title: "合约资料", panel: "data.reference" },
        { title: "合约角色", panel: "roles.diagnostics" },
      ],
    },
  ],
  panels: [
    { id: "data.reference", scope: "data", render: Reference },
    { id: "data.sync", scope: "data", render: Sync },
    { id: "data.catalog", scope: "data", render: Catalog },
  ],
};
