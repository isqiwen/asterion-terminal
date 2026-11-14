import { RequestError } from "../../src/host/action-error";
import { type ReactNode, useEffect, useState } from "react";
import { warehouseData } from "../../src/bridge/warehouse-data";
import type { DatasetChange } from "../../src/generated/asterion/warehouse_pb";
import { ServiceUnset } from "../../src/host/components/ServiceUnset";
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
  children: () => ReactNode;
}) {
  const saved = context.snapshot?.warehouse_endpoint;
  const endpoint = saved && "host" in saved ? saved : null;
  return (
    <section className="data-page" aria-label={label}>
      {endpoint ? (
        <div
          className="data-body"
          key={`${endpoint.host}:${endpoint.port}/${endpoint.fingerprint}`}
        >
          {children()}
        </div>
      ) : (
        <ServiceUnset openSettings={context.openSettings} page="warehouse" />
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
  if (!viewed || viewed === latest) return null;
  return (
    <p className="data-earlier" role="status">
      {t("正在查看旧数据版本")} <code title={viewed}>{viewed.slice(0, 8)}</code>
      {" · "}
      {t("最新数据与它相比：")}
      {change ? changeText(change) : !error && t("读取中")}
      <RequestError error={error} namespace="asterion.terminal.data-workbench" />{" "}
      <button type="button" onClick={back}>
        {t("回到最新数据")}
      </button>
    </p>
  );
}
