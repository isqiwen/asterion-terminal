import { ServiceStatusCard } from "../../src/host/components/ServiceStatusCard";
import { ServiceUnset } from "../../src/host/components/ServiceUnset";
import type { TerminalContext } from "../../src/host/feature-registry";
import { useMarketOpenError } from "../../src/host/market";
import { useErrorNotification } from "../../src/host/notifications";

export function MarketUnavailable({ context }: { context: TerminalContext }) {
  const openError = useMarketOpenError();
  const saved = context.snapshot?.market_endpoint;
  useErrorNotification(
    saved && "error" in saved ? "行情服务设置文件无法读取，已保留原文件供检查。" : "",
    "asterion.terminal.futures-market",
  );
  if (saved && "host" in saved)
    return (
      <ServiceStatusCard
        service="market"
        address={`${saved.host}:${saved.port}`}
        failed={openError || undefined}
        openSettings={context.openSettings}
      />
    );
  return <ServiceUnset openSettings={context.openSettings} />;
}
