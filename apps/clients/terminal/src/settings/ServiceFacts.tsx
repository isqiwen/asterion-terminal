import { useEffect, useState } from "react";
import { marketData } from "../bridge/market-data";
import { tradingCall } from "../bridge/trading";
import { warehouseData } from "../bridge/warehouse-data";
import { getLocale, translate } from "../i18n";
import { ErrorNotice, asDisplayError, type DisplayError } from "../i18n/errors";
const t = (key: string) => translate("host", key);

// A saved service's address, the build it runs, whether its protocol is this
// Terminal's, and for Warehouse the disk space its data can still use, in one
// label and value grid. Market and Warehouse refuse a different protocol, so an
// answer means it matches.
export function ServiceFacts(
  props: { address: string } & (
    { service: "market" | "warehouse" } | { account: string; protocol: string | undefined }
  ),
) {
  const [build, setBuild] = useState<{ version: string; matches: boolean; disk?: bigint }>();
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
      : service === "market"
        ? marketData({ case: "getStatus", value: {} }).then(status => ({
            version: status.version,
            matches: true,
          }))
        : warehouseData({ case: "getStatus", value: {} }).then(status => ({
            version: status.version,
            matches: true,
            disk: status.diskFreeBytes,
          }));
    read
      .then(next => active && setBuild(next))
      .catch(reason => active && setError(asDisplayError(reason)));
    return () => {
      active = false;
    };
  }, [service, account, expected]);
  return (
    <dl className="settings-facts">
      <dt>{t("地址")}</dt>
      <dd>
        <code>{props.address}</code>
      </dd>
      {error && (
        <>
          <dt>{t("状态")}</dt>
          <dd>
            <ErrorNotice error={error} namespace="host" />
          </dd>
        </>
      )}
      {build && <Build build={build} />}
    </dl>
  );
}

function Build({ build }: { build: { version: string; matches: boolean; disk?: bigint } }) {
  return (
    <>
      <dt>{t("服务版本")}</dt>
      <dd>{build.version || "—"}</dd>
      <dt>{t("协议")}</dt>
      <dd data-mismatch={!build.matches || undefined}>
        {t(build.matches ? "与本机一致" : "不一致：请在服务主机安装同一版本")}
      </dd>
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
