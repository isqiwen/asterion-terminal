import { useState } from "react";
import { warehouseData } from "../../src/bridge/warehouse-data";
import { Table } from "../../src/generated/asterion/warehouse_pb";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useWorkspaceDraft } from "../../src/host/workspace/drafts";
import { ErrorNotice } from "../../src/i18n/errors";
import { VersionBanner, WarehousePage } from "./Page";
import { ReferenceTable } from "./ReferenceTable";
import { StoredContracts } from "./StoredContracts";
import { exchanges, t, tableName, usePolled } from "./dataset";

const references = [Table.CONTRACTS, Table.SETTLEMENT, Table.PRICE_TICKS, Table.CALENDAR];

// The stored contracts with their charts, and the reference tables, in the
// version current when the view opened or the one opened from a run.
export function Browse(context: TerminalContext) {
  return (
    <WarehousePage context={context} label={t("数据浏览")}>
      {() => <BrowseBody context={context} />}
    </WarehousePage>
  );
}

function BrowseBody({ context }: { context: TerminalContext }) {
  const [viewed, setViewed] = useState(context.workspaceParams?.version ?? "");
  const [shown, setShown] = useWorkspaceDraft<"contracts" | Table>("browse-shown", "contracts");
  const state = usePolled(
    async () => {
      const current = await warehouseData({ case: "getDatasetSummary", value: {} });
      const latest = current.dataset?.version ?? "";
      const version = viewed || latest;
      // Reference tables open on an exchange that has data.
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
  const latest = state.value?.latest ?? "";
  return (
    <>
      <VersionBanner viewed={viewed} latest={latest} back={() => setViewed("")} />
      <div className="data-browse">
        <ErrorNotice error={state.error} />
        <div className="data-toolbar">
          <div className="segmented" role="group" aria-label={t("浏览")}>
            <button
              type="button"
              aria-pressed={shown === "contracts"}
              onClick={() => setShown("contracts")}
            >
              {t("已采集合约")}
            </button>
            {references.map(table => (
              <button
                key={table}
                type="button"
                aria-pressed={shown === table}
                onClick={() => setShown(table)}
              >
                {tableName(table)}
              </button>
            ))}
          </div>
        </div>
        {!state.value ? (
          <p className="subtle" role="status">
            {t("读取中")}
          </p>
        ) : !state.value.version ? (
          <p className="subtle">{t("还没有数据")}</p>
        ) : shown === "contracts" ? (
          <StoredContracts key={viewed || latest} version={viewed} context={context} />
        ) : (
          <ReferenceTable
            key={`${state.value.version}/${shown}`}
            version={state.value.version}
            table={shown}
            name={tableName(shown)}
            exchanges={state.value.exchanges}
          />
        )}
      </div>
    </>
  );
}
