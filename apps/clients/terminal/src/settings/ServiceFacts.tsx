import { useEffect, useState } from "react";
import { marketData } from "../bridge/market-data";
import { tradingCall } from "../bridge/trading";
import { warehouseData } from "../bridge/warehouse-data";
import { getLocale, translate } from "../i18n";
import {
  ErrorNotice,
  asDisplayError,
  isDeselectedService,
  type DisplayError,
} from "../i18n/errors";
import { useErrorNotification } from "../host/notifications";
import { StatusBadge } from "../ui/StatusBadge";
const t = (key: string) => translate("host", key);

export type ServiceBuild = { version: string; disk?: bigint };
export type ServiceTarget =
  { service: "market" | "warehouse"; refreshKey: string } | { account: string; refreshKey: string };

// Reads a saved service's build and, for Warehouse, remaining disk space.
// Protocol mismatches are refused and reported in notifications.
export function useServiceBuild(target: ServiceTarget | null) {
  const [build, setBuild] = useState<ServiceBuild>();
  const [error, setError] = useState<DisplayError>("");
  const service = target && "service" in target ? target.service : null;
  const account = target && "account" in target ? target.account : null;
  const refreshKey = target?.refreshKey ?? "";
  useErrorNotification(
    error,
    `asterion.terminal.${account ? "trading" : service === "market" ? "futures-market" : "data-workbench"}`,
  );
  useEffect(() => {
    if (!service && !account) {
      setBuild(undefined);
      setError("");
      return;
    }
    let active = true;
    setBuild(undefined);
    setError("");
    const read = account
      ? tradingCall(account, { case: "getHealth", value: {} }).then(health => ({
          version: health.version,
        }))
      : service === "market"
        ? marketData({ case: "getStatus", value: {} }).then(status => ({
            version: status.version,
          }))
        : warehouseData({ case: "getStatus", value: {} }).then(status => ({
            version: status.version,
            disk: status.diskFreeBytes,
          }));
    read
      .then(next => active && setBuild(next))
      .catch(reason => active && !isDeselectedService(reason) && setError(asDisplayError(reason)));
    return () => {
      active = false;
    };
  }, [service, account, refreshKey]);
  return { build, error };
}

export function ServiceStatusBadge({
  build,
  error,
}: {
  build?: ServiceBuild;
  error: DisplayError;
}) {
  if (error) return <StatusBadge tone="danger">{t("连接失败")}</StatusBadge>;
  if (build) return <StatusBadge tone="ok">{t("已连接")}</StatusBadge>;
  return null;
}

export function ServiceFacts({
  address,
  build,
  error,
}: {
  address: string;
  build?: ServiceBuild;
  error: DisplayError;
}) {
  return (
    <dl className="settings-facts">
      <dt>{t("地址")}</dt>
      <dd>
        <code>{address}</code>
      </dd>
      {error && (
        <>
          <dt>{t("状态")}</dt>
          <dd>
            <ErrorNotice error={error} />
          </dd>
        </>
      )}
      {build && (
        <>
          <dt>{t("服务版本")}</dt>
          <dd>
            <code>{build.version || "—"}</code>
          </dd>
          {build.disk !== undefined && (
            <>
              <dt>{t("磁盘可用")}</dt>
              <dd>
                <code>
                  {(Number(build.disk / 2n ** 20n) / 1024).toLocaleString(getLocale(), {
                    maximumFractionDigits: 1,
                  })}{" "}
                  GiB
                </code>
              </dd>
            </>
          )}
        </>
      )}
    </dl>
  );
}
