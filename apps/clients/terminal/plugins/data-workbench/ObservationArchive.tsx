import "./data.css";
import { useEffect, useRef, useState } from "react";
import {
  translate,
  getLocale,
  asDisplayError,
  ErrorNotice,
  type DisplayError,
  type TerminalContext,
} from "../contract";
import type { MarketObservation, ObservationArchive as Archive } from "../../src/bridge/client";
const t = (key: string) => translate("asterion.terminal.data-workbench", key);
const time = (ms: number | string) =>
  new Date(Number(ms)).toLocaleString(getLocale(), { timeZone: "Asia/Shanghai", hour12: false });
export function ObservationArchive({ context }: { context: TerminalContext }) {
  const [instrument, setInstrument] = useState("");
  const [id, setId] = useState("");
  const [preview, setPreview] = useState<MarketObservation | null>(null);
  const [status, setStatus] = useState<Archive | null>(null);
  const [consent, setConsent] = useState(false);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const epoch = useRef({ value: 0 });
  const market = context.snapshot?.market;
  const data = context.snapshot?.data;
  const marketKey = `${market?.service}/${market?.instance_id}`;
  const dataKey = data?.connection_id;
  useEffect(() => {
    ++epoch.current.value;
    setPreview(null);
    setConsent(false);
    setPending(false);
  }, [marketKey, instrument]);
  useEffect(() => {
    ++epoch.current.value;
    setStatus(null);
    setPreview(null);
    setConsent(false);
    setPending(false);
    setError("");
  }, [dataKey]);
  useEffect(() => {
    const token = epoch.current;
    return () => {
      ++token.value;
    };
  }, []);
  const choices = market?.subscriptions.map(row => `${row.venue}/${row.symbol}`) ?? [];
  const selected = instrument || choices[0] || "";
  async function run(action: (current: number) => Promise<void>) {
    const current = ++epoch.current.value;
    setPending(true);
    setError("");
    try {
      await action(current);
    } catch (reason) {
      if (current === epoch.current.value) setError(asDisplayError(reason));
    } finally {
      if (current === epoch.current.value) setPending(false);
    }
  }
  function fixedRead(current: number) {
    return context.query("data.observation.read", { id }).then(reply => {
      if (!reply.observation_archive || reply.observation_archive.id !== id)
        throw Error(t("归档响应身份不符，请重新读取"));
      if (current === epoch.current.value) setStatus(reply.observation_archive);
    });
  }
  const shown = status?.capture ?? preview;
  return (
    <section className="observation-archive" aria-label={t("观察归档")}>
      <h3>{t("观察归档")}</h3>
      <p>{t("仅保存已观察的分钟聚合，不是完整历史，不包含逐笔事件，最后一分钟可能尚未结束。")}</p>
      <p>
        {t("当前 Data")}：
        <code>
          {data?.node_id ?? "—"}/{data?.service ?? "—"}
        </code>
      </p>
      {!data?.online && (
        <button
          disabled={pending || context.busy}
          onClick={() =>
            void run(async () => {
              await context.trade("node.data.local.open", {});
            })
          }
        >
          {t("使用本机 Data")}
        </button>
      )}
      <label>
        {t("观察合约")}
        <select
          value={selected}
          disabled={pending}
          onChange={event => setInstrument(event.target.value)}
        >
          {!choices.length && <option value="">{t("暂无已订阅合约")}</option>}
          {choices.map(choice => (
            <option key={choice} value={choice}>
              {choice}
            </option>
          ))}
        </select>
      </label>
      <button
        disabled={pending || !market?.transport_online || !selected}
        onClick={() =>
          void run(async current => {
            const [venue, symbol] = selected.split("/");
            const reply = await context.query("market.observation.preview", { venue, symbol });
            if (!reply.market_observation) throw Error(t("归档响应身份不符，请重新读取"));
            if (current === epoch.current.value) {
              setPreview(reply.market_observation);
              setStatus(null);
              setConsent(false);
              setId(crypto.randomUUID());
            }
          })
        }
      >
        {t("固定观察预览")}
      </button>
      <label>
        {t("归档操作 ID")}
        <input
          value={id}
          disabled={pending}
          onChange={event => {
            setId(event.target.value);
            setStatus(null);
            setConsent(false);
          }}
        />
      </label>
      <button disabled={pending || !data?.online || !id} onClick={() => void run(fixedRead)}>
        {t("从 Data 读取固定原件")}
      </button>
      {preview && !status && (
        <>
          <p>{t("预览缓存可能失效；旧原件缺失时不会自动采集另一份。")}</p>
          <label>
            <input
              type="checkbox"
              checked={consent}
              disabled={pending}
              onChange={event => setConsent(event.target.checked)}
            />
            {t("确认将此固定观察交给当前 Data")}
          </label>
          <button
            disabled={pending || !data?.online || !consent || !id}
            onClick={() =>
              void run(async current => {
                await context.trade("data.observation.admit", { id, source: preview.reference });
                await fixedRead(current);
                if (current !== epoch.current.value) return;
                await context.trade("data.observation.execute", { id });
                await fixedRead(current);
              })
            }
          >
            {t("接纳并发布归档")}
          </button>
        </>
      )}
      {status && (
        <section aria-label={t("归档状态")}>
          <p>{t(status.state === "published" ? "已发布固定原件" : "已接纳，尚未发布")}</p>
          <code>{status.id}</code>
          <p>
            {t("Data 接纳记录时间（上海）")}：{time(status.admitted_at_ms)}
          </p>
          {status.published_at_ms && (
            <p>
              {t("本次发布记录时间（上海）")}：{time(status.published_at_ms)}
            </p>
          )}
          <p>
            <code>
              {status.source.node}/{status.source.service}/{status.source.version}
            </code>
          </p>
          {status.state === "admitted" && (
            <>
              <p>{t("重试只读取原引用；来源缺失时保留未完成状态，不替换最新行情。")}</p>
              <button
                disabled={pending || !data?.online}
                onClick={() =>
                  void run(async current => {
                    await context.trade("data.observation.execute", { id: status.id });
                    await fixedRead(current);
                  })
                }
              >
                {t("继续原归档操作")}
              </button>
            </>
          )}
        </section>
      )}
      {shown && (
        <section aria-label={t(status?.capture ? "Data 固定观察原件" : "Market 固定观察预览")}>
          <p>
            {t("来源与固定版本")}：
            <code>
              {shown.reference.node}/{shown.reference.service}/{shown.reference.version}
            </code>
          </p>
          <p>
            {t("首次观察时间（上海）")}：{time(shown.minutes.first_observation_ms)}
          </p>
          <p>
            {t("冻结时间（上海）")}：{time(shown.captured_at_ms)}
          </p>
          <p>
            {t(
              shown.minutes.interrupted
                ? "观察曾中断，不能证明连续覆盖"
                : "未标记中断也不代表整日完整",
            )}
          </p>
          <p>
            {t("流身份及已应用序号")}：
            <code>
              {shown.stream_id}/{shown.applied_sequence}
            </code>
          </p>
          <p>
            {t("显示前 20 根观察聚合；总根数")}：{shown.minutes.bars.length}
          </p>
          <table aria-label={t("固定观察分钟")}>
            <thead>
              <tr>
                {["时间（上海）", "开", "高", "低", "收", "观察量"].map(label => (
                  <th key={label}>{t(label)}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {shown.minutes.bars.slice(0, 20).map(bar => (
                <tr key={bar.start_ms}>
                  <td>{time(bar.start_ms)}</td>
                  <td>{bar.open}</td>
                  <td>{bar.high}</td>
                  <td>{bar.low}</td>
                  <td>{bar.close}</td>
                  <td>{bar.volume}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>
      )}
      {pending && <p role="status">{t("正在核对固定归档…")}</p>}
      {error && (
        <p role="alert">
          <ErrorNotice error={error} />
        </p>
      )}
    </section>
  );
}
