import { ServiceStatusCard } from "../../src/host/components/ServiceStatusCard";
import { ServiceUnset } from "../../src/host/components/ServiceUnset";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useMarketOpenError } from "../../src/host/market";
import { translate } from "../../src/i18n";
const t = (key: string) => translate("asterion.terminal.futures-market", key);

export function MarketUnavailable({ context }: { context: TerminalContext }) {
  const openError = useMarketOpenError();
  const saved = context.snapshot?.market_endpoint;
  if (saved && "host" in saved)
    return (
      <ServiceStatusCard
        service="market"
        address={`${saved.host}:${saved.port}`}
        failed={openError || undefined}
        openSettings={context.openSettings}
      />
    );
  return (
    <>
      {saved && "error" in saved && (
        <p className="action-error" role="alert">
          {t("行情服务设置文件无法读取，已保留原文件供检查。")}
        </p>
      )}
      <ServiceUnset openSettings={context.openSettings} />
    </>
  );
}
