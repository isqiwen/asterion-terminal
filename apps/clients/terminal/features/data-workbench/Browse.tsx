import { useEffect, useState } from "react";
import { warehouseData } from "../../src/bridge/warehouse-data";
import { Table } from "../../src/generated/asterion/warehouse_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useErrorNotification } from "../../src/host/notifications";
import { useWarehousePoll } from "../../src/host/warehouse";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { VersionBanner, WarehousePage, WarehouseUnavailable } from "./Page";
import { ReferenceTable } from "./ReferenceTable";
import { StoredContracts } from "./StoredContracts";
import { Acquisition } from "./Acquisition";
import { exchanges, referenceTables, t, tableName } from "./dataset";

// The stored contracts with their charts, and the reference tables behind them,
// in the version current when the view opened or the one opened from a run.
export function Browse(context: TerminalContext) {
  return (
    <WarehousePage context={context} label={t("数据浏览")}>
      {() => <BrowseBody context={context} />}
    </WarehousePage>
  );
}

function BrowseBody({ context }: { context: TerminalContext }) {
  const paramVersion = context.workspaceParams?.version ?? "";
  const paramTable = context.workspaceParams?.table ?? "";
  const [viewed, setViewed] = useState(paramVersion);
  const [tab, setTab] = useWorkspaceDraft<"contracts" | Table>("browse-tab", "contracts");
  const [acquiring, setAcquiring] = useState<Table>();
  const options = useWarehousePoll(
    () => warehouseData({ case: "getAcquisitionOptions", value: {} }),
    [],
    true,
  );
  useEffect(() => {
    if (!paramVersion && !paramTable) return;
    if (paramVersion) setViewed(paramVersion);
    const parsed = Number(paramTable);
    if ((referenceTables as readonly Table[]).includes(parsed as Table)) setTab(parsed as Table);
    context.navigate("workspace.data-browse", {});
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [paramVersion, paramTable]);
  const state = useWarehousePoll(
    async () => {
      const current = await warehouseData({ case: "getDatasetSummary", value: {} });
      const latest = current.dataset?.version ?? "";
      const version = viewed || latest;
      // Parameter and listing tables open on an exchange that has data.
      const coverage = version
        ? await warehouseData({ case: "getDatasetCoverage", value: { version } })
        : undefined;
      const held = new Set(
        [
          ...(coverage?.products ?? []).map(item => item.product),
          ...(coverage?.contracts ?? []).map(item => item.contract),
        ].map(id => id.split("/")[0]),
      );
      return {
        latest,
        version,
        exchanges: [
          ...exchanges.filter(item => held.has(item)),
          ...exchanges.filter(item => !held.has(item)),
        ],
      };
    },
    [viewed],
    true,
  );
  useErrorNotification(state.error, "asterion.terminal.data-workbench");
  useErrorNotification(options.error, "asterion.terminal.data-workbench");
  const latest = state.value?.latest ?? "";
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
        <div className="data-toolbar">
          <div className="tabs" role="group" aria-label={t("浏览")}>
            <button
              type="button"
              aria-pressed={tab === "contracts"}
              onClick={() => setTab("contracts")}
            >
              {t("已采集合约")}
            </button>
            {referenceTables.map(item => (
              <button
                key={item}
                type="button"
                aria-pressed={tab === item}
                onClick={() => setTab(item)}
              >
                {tableName(item)}
              </button>
            ))}
          </div>
          {!viewed && options.value?.tables.some(item => item.table === tab) && (
            <button type="button" onClick={() => setAcquiring(tab as Table)}>
              {t("采集")}
            </button>
          )}
        </div>
        {!state.value ? (
          state.error ? (
            <WarehouseUnavailable
              error={state.error}
              retry={() => void state.refresh()}
              openSettings={context.openSettings}
            />
          ) : (
            <p className="subtle" role="status">
              {t("读取中")}
            </p>
          )
        ) : !state.value.version ? (
          <p className="subtle">{t("还没有数据")}</p>
        ) : tab === "contracts" ? (
          <StoredContracts
            key={state.value.version}
            version={state.value.version}
            context={context}
          />
        ) : (
          <ReferenceTable
            key={`${state.value.version}/${tab}`}
            version={state.value.version}
            table={tab}
            name={tableName(tab)}
            exchanges={state.value.exchanges}
          />
        )}
      </div>
      {acquiring !== undefined && options.value && (
        <Acquisition
          table={acquiring}
          name={tableName(acquiring)}
          options={options.value}
          products={[]}
          known={[]}
          preset={{}}
          close={() => setAcquiring(undefined)}
          started={() => {
            setAcquiring(undefined);
            void state.refresh();
          }}
        />
      )}
    </>
  );
}
