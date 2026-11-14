import { useEffect, useState } from "react";
import { marketData } from "../bridge/market-data";
import { tradingCall } from "../bridge/trading";
import { warehouseData } from "../bridge/warehouse-data";
import { getLocale, translate } from "../i18n";
import { asDisplayError, type DisplayError } from "../i18n/errors";
import { useErrorNotification } from "../host/notifications";
const t = (key: string) => translate("host", key);

// A saved service's address, the build it runs and, for Warehouse, the disk
// space its data can still use, in one label and value grid. A service of
// another protocol is refused and reported in notifications.
export function ServiceFacts(
  props: { address: string } & ({ service: "market" | "warehouse" } | { account: string }),
) {
  const [build, setBuild] = useState<{ version: string; disk?: bigint }>();
  const [error, setError] = useState<DisplayError>("");
  const service = "service" in props ? props.service : null;
  const account = "account" in props ? props.account : null;
  useErrorNotification(
    error,
    `asterion.terminal.${account ? "trading" : service === "market" ? "futures-market" : "data-workbench"}`,
  );
  useEffect(() => {
    let active = true;
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
      .catch(reason => active && setError(asDisplayError(reason)));
    return () => {
      active = false;
    };
  }, [service, account]);
  return (
    <dl className="settings-facts">
      <dt>{t("地址")}</dt>
      <dd>
        <code>{props.address}</code>
      </dd>
      {error && (
        <>
          <dt>{t("状态")}</dt>
          <dd>{t("中断")}</dd>
        </>
      )}
      {build && <Build build={build} />}
    </dl>
  );
}

function Build({ build }: { build: { version: string; disk?: bigint } }) {
  return (
    <>
      <dt>{t("服务版本")}</dt>
      <dd>{build.version || "—"}</dd>
      {build.disk !== undefined && (
        <>
          <dt>{t("磁盘可用")}</dt>
          <dd>
            {(Number(build.disk / 2n ** 20n) / 1024).toLocaleString(getLocale(), {
              maximumFractionDigits: 1,
            })}{" "}
            GiB
          </dd>
        </>
      )}
    </>
  );
}
