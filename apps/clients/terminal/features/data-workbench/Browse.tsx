import { useEffect, useState } from "react";
import { warehouseData } from "../../src/bridge/warehouse-data";
import { Table } from "../../src/generated/asterion/warehouse_pb";
import { ServiceStatusCard } from "../../src/host/components/ServiceStatusCard";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useErrorNotification } from "../../src/host/notifications";
import { useWarehousePoll } from "../../src/host/warehouse";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { VersionBanner, WarehousePage } from "./Page";
import { ReferenceTable } from "./ReferenceTable";
import { exchanges, t, tableName } from "./dataset";

// Reference tables maintained by Warehouse: fundamentals and trading parameters.
const groups: readonly { name: string; tabs: readonly Table[] }[] = [
  { name: "基本面", tabs: [Table.WAREHOUSE_RECEIPTS, Table.MEMBER_POSITIONS, Table.INVENTORY] },
  {
    name: "交易参数",
    tabs: [Table.SETTLEMENT, Table.PRICE_TICKS, Table.CONTRACTS, Table.CALENDAR],
  },
];

// Follow the latest data unless a run or activity opens a fixed version.
export function Browse(context: TerminalContext) {
  return (
    <WarehousePage context={context} label={t("数据浏览")}>
      {address => <BrowseBody context={context} address={address} />}
    </WarehousePage>
  );
}

function BrowseBody({ context, address }: { context: TerminalContext; address: string }) {
  const paramVersion = context.workspaceParams?.version ?? "";
  const paramTable = context.workspaceParams?.table ?? "";
  const [viewed, setViewed] = useState(paramVersion);
  const [tab, setTab] = useWorkspaceDraft<Table>("browse-tab", Table.WAREHOUSE_RECEIPTS);
  useEffect(() => {
    if (!paramVersion && !paramTable) return;
    if (paramVersion) setViewed(paramVersion);
    const parsed = Number(paramTable);
    if (parsed && groups.some(item => item.tabs.includes(parsed as Table))) setTab(parsed as Table);
    context.navigate("workspace.data-browse", {});
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [paramVersion, paramTable]);
  const [ordered, setOrdered] = useState<{ version: string; exchanges: string[] }>({
    version: "",
    exchanges: [...exchanges],
  });
  const state = useWarehousePoll(
    async () => {
      const current = await warehouseData({ case: "getDatasetSummary", value: {} });
      const latest = current.dataset?.version ?? "";
      const version = viewed || latest;
      return { latest, version };
    },
    [viewed],
    !!viewed,
  );
  const activeVersion = state.value?.version ?? "";
  useEffect(() => {
    if (!activeVersion || ordered.version === activeVersion) return;
    let active = true;
    // Open on an exchange that already holds data.
    void warehouseData({ case: "getDatasetCoverage", value: { version: activeVersion } })
      .then(coverage => {
        if (!active) return;
        const held = new Set(
          coverage.products
            .filter(item => item.daily?.held || item.minutes?.held)
            .map(item => item.product.split("/")[0]),
        );
        setOrdered({
          version: activeVersion,
          exchanges: [
            ...exchanges.filter(item => held.has(item)),
            ...exchanges.filter(item => !held.has(item)),
          ],
        });
      })
      .catch(() => {});
    return () => {
      active = false;
    };
  }, [activeVersion, ordered.version]);
  useErrorNotification(state.error, "asterion.terminal.data-workbench");
  if (!state.value)
    return (
      <ServiceStatusCard
        service="warehouse"
        address={address}
        failed={state.error || undefined}
        openSettings={context.openSettings}
      />
    );
  const latest = state.value.latest;
  const group = groups.find(item => item.tabs.includes(tab)) ?? groups[0]!;
  const activeTab = group.tabs.includes(tab) ? tab : group.tabs[0]!;
  const nav = (
    <div className="data-browse-nav">
      <div className="data-browse-groups" role="group" aria-label={t("数据分类")}>
        {groups.map(item => (
          <button
            key={item.name}
            type="button"
            aria-pressed={item === group}
            onClick={() => item !== group && setTab(item.tabs[0]!)}
          >
            {t(item.name)}
          </button>
        ))}
      </div>
      <div className="data-browse-tables" role="group" aria-label={t(group.name)}>
        {group.tabs.map(item => (
          <button
            key={item}
            type="button"
            aria-pressed={activeTab === item}
            onClick={() => setTab(item)}
          >
            {tableName(item)}
          </button>
        ))}
      </div>
    </div>
  );
  return (
    <>
      <VersionBanner
        viewed={viewed}
        latest={latest}
        back={() => {
          setViewed("");
          context.navigate("workspace.data-browse", {});
        }}
      />
      <div className="data-browse">
        {!state.value.version ? (
          <>
            <div className="data-toolbar">{nav}</div>
            <p className="subtle">{t("还没有数据")}</p>
          </>
        ) : (
          <ReferenceTable
            key={`${viewed}/${activeTab}`}
            version={viewed || state.value.version}
            table={activeTab}
            name={tableName(activeTab)}
            exchanges={ordered.exchanges}
            toolbar={nav}
          />
        )}
      </div>
    </>
  );
}
