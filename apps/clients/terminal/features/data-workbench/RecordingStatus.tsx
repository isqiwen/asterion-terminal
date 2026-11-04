import { useEffect, useState } from "react";
import type { Status } from "../../src/generated/asterion/recording_pb";
import { useMarket } from "../../src/host/market";
import {
  ActionError,
  ErrorNotice,
  useActionError,
  asDisplayError,
  getLocale,
  marketData,
  translate,
  type DisplayError,
  type TerminalContext,
} from "../contract";

const t = (key: string) => translate("asterion.terminal.data-workbench", key);
const phases: Record<string, string> = {
  connected: "行情已连接",
  connecting: "连接中",
  initializing: "初始化中",
  logging_in: "登录中",
  reconnecting: "重连中",
  disconnected: "行情未连接",
  error: "行情连接失败",
};
const clock = (value: bigint) =>
  value > 0n
    ? new Date(Number(value)).toLocaleString(getLocale(), {
        timeZone: "Asia/Shanghai",
        hour12: false,
      })
    : "—";
const size = (bytes: bigint) =>
  `${(Number(bytes / 1024n) / 1024).toLocaleString(getLocale(), { maximumFractionDigits: 1 })} MiB`;

// Only this client's status reads are polled. Changing or closing its Market
// connection discards pending replies without changing the recorder's lifetime.
export function RecordingStatus({ context }: { context: TerminalContext }) {
  const market = useMarket();
  const instance = market?.snapshot.instanceId;
  const online = market?.online;
  const saved = context.snapshot?.market_endpoint;
  const address = saved && "host" in saved ? `${saved.host}:${saved.port}` : "";
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState<DisplayError>("");
  const { error: opened, attempt } = useActionError();
  useEffect(() => {
    setStatus(null);
    setError("");
    if (!instance || !online) return;
    let active = true;
    let pending = false;
    const read = async () => {
      if (pending) return;
      pending = true;
      try {
        const next = await marketData({ case: "recordingStatus", value: {} }, "recordingStatus");
        if (!active) return;
        if (next.instance !== instance) throw new Error(t("录制状态不属于当前行情服务"));
        setStatus(next);
        setError("");
      } catch (reason) {
        if (active) {
          setStatus(null);
          setError(asDisplayError(reason));
        }
      } finally {
        pending = false;
      }
    };
    void read();
    const timer = window.setInterval(() => {
      if (!document.hidden) void read();
    }, 3000);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, [address, instance, online]);

  return (
    <section className="data-card recording-status" aria-label={t("录制监控")}>
      <header>
        <h3>{t("录制监控")}</h3>
        {status && (
          <span role="status" data-phase={status.phase}>
            {t(phases[status.phase] ?? status.phase)}
          </span>
        )}
      </header>
      {!market ? (
        <div className="data-actions">
          <button
            disabled={context.busy}
            onClick={() => void attempt(() => context.trade("market.open"))}
          >
            {t("连接行情服务")}
          </button>
          <span className="subtle">{t("连接行情服务后查看录制状态。")}</span>
          <ActionError error={opened} />
        </div>
      ) : !online ? (
        <p role="status">{t("行情服务失联，录制状态未知。")}</p>
      ) : error ? (
        <ErrorNotice error={error} />
      ) : !status ? (
        <p className="subtle">{t("读取中")}</p>
      ) : (
        <>
          <dl>
            <div>
              <dt>{t("交易日")}</dt>
              <dd>{status.tradingDay || "—"}</dd>
            </div>
            <div>
              <dt>{t("订阅合约")}</dt>
              <dd>{status.subscribed.toLocaleString(getLocale())}</dd>
            </div>
            <div>
              <dt>{t("本次启动写入 Tick")}</dt>
              <dd>{status.rows.toLocaleString(getLocale())}</dd>
            </div>
            <div>
              <dt>{t("写入文件")}</dt>
              <dd>{status.files.toLocaleString(getLocale())}</dd>
            </div>
            <div>
              <dt>{t("写入大小")}</dt>
              <dd>{size(status.bytes)}</dd>
            </div>
            <div>
              <dt>{t("磁盘可用空间")}</dt>
              <dd>{size(status.diskFreeBytes)}</dd>
            </div>
            <div>
              <dt>{t("最后收到行情（北京时间）")}</dt>
              <dd>{clock(status.lastQuoteMs)}</dd>
            </div>
            <div>
              <dt>{t("最后落盘（北京时间）")}</dt>
              <dd>{clock(status.writtenMs)}</dd>
            </div>
          </dl>
          {(status.lost > 0n || status.writeError) && (
            <p role="alert">
              {t("录制写入异常，部分数据可能丢失。")} {t("丢失 Tick")}:{" "}
              {status.lost.toLocaleString(getLocale())} {status.writeError}
            </p>
          )}
          {status.catalogError && (
            <p role="alert">
              {t("合约目录查询失败")}: {status.catalogError}
            </p>
          )}
          {!!status.errorCode && (
            <p role="alert">
              {t("行情连接失败")}: CTP {status.errorCode}
            </p>
          )}
          {status.merging && (
            <p>
              {t("正在合并交易日")}: {status.merging}
            </p>
          )}
          {status.mergeError && (
            <p role="alert">
              {t("历史文件合并失败")}: {status.mergeError}
            </p>
          )}
        </>
      )}
      <p className="subtle">
        {t(
          "录制由行情服务独立运行；关闭 Terminal 不会停止。计数仅代表当前实例，不保证历史时段完整。",
        )}
      </p>
    </section>
  );
}
