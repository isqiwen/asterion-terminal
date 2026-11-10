import { useEffect, useState } from "react";
import { marketData } from "../bridge/market-data";
import { tradingCall } from "../bridge/trading";
import { warehouseData } from "../bridge/warehouse-data";
import { translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
const t = (key: string) => translate("host", key);

// The build a saved service runs and whether its protocol is this Terminal's.
// Market and Warehouse refuse a different protocol, so an answer means it matches.
export function ServiceBuild(
  props: { service: "market" | "warehouse" } | { account: string; protocol: string | undefined },
) {
  const [build, setBuild] = useState<{ version: string; matches: boolean }>();
  const [error, setError] = useState<DisplayError>("");
  const service = "service" in props ? props.service : null;
  const account = "account" in props ? props.account : null;
  const expected = "protocol" in props ? props.protocol : undefined;
  useEffect(() => {
    let active = true;
    const read = account
      ? tradingCall(account, { case: "getHealth", value: {} }).then(health => ({
          version: health.version,
          matches: health.protocol === expected,
        }))
      : (service === "market"
          ? marketData({ case: "getStatus", value: {} })
          : warehouseData({ case: "getStatus", value: {} })
        ).then(status => ({ version: status.version, matches: true }));
    read
      .then(next => active && setBuild(next))
      .catch(reason => active && setError(asDisplayError(reason)));
    return () => {
      active = false;
    };
  }, [service, account, expected]);
  if (error) return <ErrorNotice error={error} namespace="host" />;
  if (!build) return null;
  return (
    <dl className="settings-facts">
      <dt>{t("服务版本")}</dt>
      <dd>{build.version || "—"}</dd>
      <dt>{t("协议")}</dt>
      <dd data-mismatch={!build.matches || undefined}>
        {t(build.matches ? "与本机一致" : "不一致：请在服务主机安装同一版本")}
      </dd>
    </dl>
  );
}
