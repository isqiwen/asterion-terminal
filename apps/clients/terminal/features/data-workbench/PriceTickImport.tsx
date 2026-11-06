import { useState } from "react";
import { decimal } from "../../src/bridge/decimal";
import {
  ErrorNotice,
  asDisplayError,
  marketData,
  translate,
  type DisplayError,
  type MessageValues,
} from "../contract";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const record = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

function parse(text: string) {
  const value: unknown = JSON.parse(text);
  if (
    !record(value) ||
    typeof value.exchange !== "string" ||
    !["SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"].includes(value.exchange) ||
    !Array.isArray(value.rows) ||
    !value.rows.length
  )
    throw new Error(t("文件需要交易所和非空的历史规则列表。"));
  const rows = value.rows.map((row: unknown) => {
    if (
      !record(row) ||
      typeof row.contract !== "string" ||
      typeof row.effectiveFrom !== "string" ||
      typeof row.effectiveUntil !== "string" ||
      typeof row.priceTick !== "string" ||
      typeof row.sourceUrl !== "string" ||
      !/^\d{4}-\d{2}-\d{2}$/.test(row.effectiveFrom) ||
      !/^\d{4}-\d{2}-\d{2}$/.test(row.effectiveUntil)
    )
      throw new Error(t("每条规则需要合约、生效日期、失效日期、报价单位字符串和来源网址。"));
    return {
      contract: row.contract,
      effectiveFrom: row.effectiveFrom,
      effectiveUntil: row.effectiveUntil,
      priceTick: decimal(row.priceTick),
      sourceUrl: row.sourceUrl,
    };
  });
  return { exchange: value.exchange, rows };
}

type Prepared = ReturnType<typeof parse> & { filename: string };

export function PriceTickImport({
  onImported,
}: {
  onImported: (dataset: { provider: string; version: string }) => void;
}) {
  const [prepared, setPrepared] = useState<Prepared>();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<DisplayError>("");
  const [imported, setImported] = useState(false);
  const read = async (file: File) => {
    setBusy(true);
    setPrepared(undefined);
    setImported(false);
    setError("");
    try {
      setPrepared({ ...parse(await file.text()), filename: file.name });
    } catch (failure) {
      setError(
        failure instanceof SyntaxError ? t("文件不是有效的 JSON。") : asDisplayError(failure),
      );
    } finally {
      setBusy(false);
    }
  };
  const submit = async () => {
    if (!prepared) return;
    setBusy(true);
    setError("");
    try {
      const dataset = await marketData({
        case: "importPriceTicks",
        value: { exchange: prepared.exchange, rows: prepared.rows },
      });
      setImported(true);
      onImported(dataset);
    } catch (failure) {
      setError(asDisplayError(failure));
    } finally {
      setBusy(false);
    }
  };
  const from = prepared?.rows.map(row => row.effectiveFrom).sort()[0];
  const until = prepared?.rows
    .map(row => row.effectiveUntil)
    .sort()
    .at(-1);
  return (
    <section className="data-card" aria-label={t("导入历史报价单位")}>
      <header>
        <h3>{t("导入历史报价单位")}</h3>
      </header>
      <p className="subtle">{t("仅导入已核对官方来源的历史规则。")}</p>
      <label>
        {t("历史规则文件（JSON）")}
        <input
          type="file"
          accept=".json,application/json"
          disabled={busy}
          onChange={event => {
            const file = event.target.files?.[0];
            if (file) void read(file);
          }}
        />
      </label>
      {prepared && (
        <>
          <p>{prepared.filename}</p>
          <p>
            {t("{exchange} · {count} 条 · {from} 至 {until}（不含）", {
              exchange: prepared.exchange,
              count: prepared.rows.length,
              from: from!,
              until: until!,
            })}
          </p>
          <p>{t("将替换该交易所已导入的历史规则，已固定的旧版本不变。")}</p>
          <div className="data-actions">
            <button className="primary" disabled={busy || imported} onClick={() => void submit()}>
              {t(busy ? "正在导入…" : "导入")}
            </button>
            {imported && <span role="status">{t("已导入")}</span>}
          </div>
        </>
      )}
      <ErrorNotice error={error} />
    </section>
  );
}
