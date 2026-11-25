import { type ReactNode, useEffect, useState } from "react";
import { warehouseData } from "../../src/bridge/warehouse-data";
import type { DatasetChange } from "../../src/generated/asterion/warehouse_pb";
import { ServiceStatusCard } from "../../src/host/components/ServiceStatusCard";
import { ServiceUnset } from "../../src/host/components/ServiceUnset";
import { useErrorNotification } from "../../src/host/notifications";
import { useWarehouse } from "../../src/host/warehouse";
import { type DisplayError, asDisplayError } from "../../src/i18n/errors";
import type { TerminalContext } from "../../src/host/feature-registry";
import { t } from "./dataset";

export const changeText = (change: DatasetChange) =>
  t("新增 {added} · 替换 {replaced} · 删除 {removed}", {
    added: change.added,
    replaced: change.replaced,
    removed: change.removed,
  });

// Every view reads the saved remote Warehouse; a new endpoint starts it afresh.
export function WarehousePage({
  context,
  label,
  children,
}: {
  context: TerminalContext;
  label: string;
  children: (address: string) => ReactNode;
}) {
  const saved = context.snapshot?.warehouse_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  const address = endpoint ? `${endpoint.host}:${endpoint.port}` : "";
  const key = endpoint ? `${address}/${endpoint.fingerprint}` : "";
  const warehouse = useWarehouse();
  const gated = Boolean(warehouse && warehouse.key === key && warehouse.error !== "");
  return (
    <section className="data-page" aria-label={label}>
      {!endpoint ? (
        <ServiceUnset openSettings={context.openSettings} page="warehouse" />
      ) : gated ? (
        <ServiceStatusCard
          service="warehouse"
          address={address}
          failed={warehouse?.error || undefined}
          openSettings={context.openSettings}
        />
      ) : (
        <div className="data-body" key={key}>
          {children(address)}
        </div>
      )}
    </section>
  );
}

// Shown while an earlier version, opened from a run or the activity, replaces the latest.
export function VersionBanner({
  viewed,
  latest,
  back,
}: {
  viewed: string;
  latest: string;
  back: () => void;
}) {
  const [change, setChange] = useState<DatasetChange>();
  const [error, setError] = useState<DisplayError>("");
  useEffect(() => {
    let active = true;
    setChange(undefined);
    setError("");
    if (viewed && latest && viewed !== latest)
      void warehouseData({
        case: "compareDatasetVersions",
        value: { version: latest, base: viewed },
      })
        .then(next => active && setChange(next))
        .catch(reason => active && setError(asDisplayError(reason)));
    return () => {
      active = false;
    };
  }, [viewed, latest]);
  useErrorNotification(error, "asterion.terminal.data-workbench");
  if (!viewed || viewed === latest) return null;
  return (
    <p className="data-earlier" role="status">
      {t("正在查看旧数据版本")} <code title={viewed}>{viewed.slice(0, 8)}</code>
      {(change || !error) && (
        <>
          {" · "}
          {t("最新数据与它相比：")}
          {change ? changeText(change) : t("读取中")}
        </>
      )}{" "}
      <button type="button" onClick={back}>
        {t("回到最新数据")}
      </button>
    </p>
  );
}
