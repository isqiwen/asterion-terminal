import { useState } from "react";
import { Table } from "../../src/generated/asterion/warehouse_pb";
import {
  ActionError,
  translate,
  useActionError,
  warehouseData,
  type AcquisitionOptions,
  type MessageValues,
} from "../contract";
import { ContractPicker } from "./ContractPicker";
import { Dialog } from "./Dialog";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.data-workbench", key, values);
const day = (offset: number) => new Date(Date.now() + offset * 86400000).toISOString().slice(0, 10);
// Tables a task can name products or contracts for; minutes name nothing else.
const byInstrument = [Table.DAILY, Table.MINUTES, Table.SETTLEMENT, Table.PRICE_TICKS];

// What a coverage gap or the view's button opens the dialog with.
export type Preset = { exchange?: string; instruments?: string[]; first?: string; last?: string };

// Acquires one table. Instruments without days span each contract's listed life.
export function Acquisition({
  table,
  name,
  options,
  known,
  preset,
  close,
  started,
}: {
  table: Table;
  name: string;
  options: AcquisitionOptions;
  known: string[];
  preset: Preset;
  close: () => void;
  started: () => void;
}) {
  const minutes = table === Table.MINUTES;
  const [target, setTarget] = useState<"exchange" | "instruments">(
    minutes || preset.instruments ? "instruments" : "exchange",
  );
  const named = target === "instruments";
  const [exchange, setExchange] = useState(preset.exchange ?? options.exchanges[0] ?? "");
  const [instruments, setInstruments] = useState(preset.instruments ?? []);
  const [begin, setBegin] = useState(preset.first ?? (named ? "" : day(-30)));
  const [end, setEnd] = useState(preset.last ?? (named ? "" : day(-1)));
  const [replace, setReplace] = useState(false);
  const [submitting, setSubmitting] = useState(false);
  const failure = useActionError();
  const choose = (next: "exchange" | "instruments") => {
    setTarget(next);
    setBegin(next === "instruments" ? "" : day(-30));
    setEnd(next === "instruments" ? "" : day(-1));
  };
  const scope = named ? { exchanges: [], instruments } : { exchanges: [exchange], instruments: [] };
  const start = async () => {
    setSubmitting(true);
    const done = await failure.attempt(async () => {
      const request = { refetch: replace, ...scope, firstDay: begin, lastDay: end };
      await warehouseData({
        case: "startAcquisition",
        value: { id: crypto.randomUUID(), table, ...request },
      });
      // Market data is usable only with its price ticks.
      if (
        [Table.DAILY, Table.MINUTES, Table.SETTLEMENT].includes(table) &&
        options.tables.includes(Table.PRICE_TICKS)
      )
        await warehouseData({
          case: "startAcquisition",
          value: { id: crypto.randomUUID(), table: Table.PRICE_TICKS, ...request },
        });
    });
    setSubmitting(false);
    if (done) started();
  };
  return (
    <Dialog title={t("采集 {table}", { table: name })} close={close}>
      {byInstrument.includes(table) && !minutes && (
        <div role="radiogroup" aria-label={t("范围")} className="data-choice">
          {(["exchange", "instruments"] as const).map(item => (
            <label key={item}>
              <input
                type="radio"
                name="target"
                checked={target === item}
                onChange={() => choose(item)}
              />
              {t(item === "exchange" ? "整个交易所" : "品种或合约")}
            </label>
          ))}
        </div>
      )}
      {named ? (
        <ContractPicker known={known} value={instruments} onChange={setInstruments} />
      ) : (
        <label>
          {t("交易所")}
          <select value={exchange} onChange={event => setExchange(event.target.value)}>
            {options.exchanges.map(item => (
              <option key={item}>{item}</option>
            ))}
          </select>
        </label>
      )}
      <div className="data-fields">
        <label>
          {t("开始日期")}
          <input type="date" value={begin} onChange={event => setBegin(event.target.value)} />
        </label>
        <label>
          {t("结束日期")}
          <input type="date" value={end} onChange={event => setEnd(event.target.value)} />
        </label>
      </div>
      {named && <p className="subtle">{t("日期留空即整个上市期。")}</p>}
      <label>
        <input
          type="checkbox"
          checked={replace}
          onChange={event => setReplace(event.target.checked)}
        />
        {t("重新采集已有分区")}
      </label>
      <ActionError error={failure.error} />
      <div className="data-actions">
        <button type="button" onClick={close}>
          {t("取消")}
        </button>
        <button
          type="button"
          className="primary"
          disabled={
            submitting ||
            (begin !== "" && end !== "" && begin > end) ||
            (named ? !instruments.length : !exchange || !begin || !end)
          }
          onClick={() => void start()}
        >
          {t("开始采集")}
        </button>
      </div>
    </Dialog>
  );
}
