import { DateInput } from "../../src/ui/DateInput";
import { useState } from "react";
import { Table } from "../../src/generated/asterion/warehouse_pb";
import { warehouseData } from "../../src/bridge/warehouse-data";
import type { AcquisitionOptions } from "../../src/generated/asterion/warehouse_pb";
import { ActionError, useActionError } from "../../src/host/action-error";
import { ContractPicker } from "./ContractPicker";
import { Dialog } from "./Dialog";
import { beijingDay, exchanges, t } from "./dataset";

const day = beijingDay;

// What a coverage gap or the view's button opens the dialog with.
export type Preset = {
  exchanges?: string[];
  instruments?: string[];
  first?: string;
  last?: string;
};

// Daily bars are acquired whole-day per exchange; minute bars are acquired by
// product (all listed contracts in the span) or individual contract.
export function Acquisition({
  table,
  name,
  options,
  products,
  known,
  preset,
  close,
  started,
}: {
  table: Table;
  name: string;
  options: AcquisitionOptions;
  products: string[];
  known: string[];
  preset: Preset;
  close: () => void;
  started: () => void;
}) {
  const minutes = table === Table.MINUTES;
  const offeredExchanges = [
    ...exchanges.filter(item => options.exchanges.includes(item)),
    ...options.exchanges.filter(item => !exchanges.includes(item)),
  ];
  const initialExchanges = preset.exchanges?.filter(item => offeredExchanges.includes(item));
  const [chosenExchanges, setExchanges] = useState(
    initialExchanges?.length ? initialExchanges : offeredExchanges.slice(0, 1),
  );
  const [instruments, setInstruments] = useState(preset.instruments ?? []);
  const [begin, setBegin] = useState(preset.first ?? (minutes ? "" : day(-30)));
  const [end, setEnd] = useState(preset.last ?? (minutes ? "" : day(-1)));
  const [replace, setReplace] = useState(false);
  const [submitting, setSubmitting] = useState(false);
  const failure = useActionError();
  const disabled =
    submitting ||
    (begin !== "" && end !== "" && begin > end) ||
    (minutes ? !instruments.length : !chosenExchanges.length || !begin || !end);
  const start = async () => {
    if (disabled) return;
    setSubmitting(true);
    const done = await failure.attempt(async () => {
      await warehouseData({
        case: "startAcquisition",
        value: {
          id: `acq-${crypto.randomUUID().replace(/-/g, "").slice(0, 12)}`,
          table,
          refetch: replace,
          exchanges: minutes ? [] : chosenExchanges,
          instruments: minutes ? instruments : [],
          firstDay: begin,
          lastDay: end,
        },
      });
    });
    setSubmitting(false);
    if (done) started();
  };
  return (
    <Dialog title={t("采集 {table}", { table: name })} close={close}>
      <form
        className="data-dialog-form"
        onSubmit={event => {
          event.preventDefault();
          void start();
        }}
      >
        {minutes ? (
          <ContractPicker
            products={products}
            known={known}
            focusExchange={preset.exchanges?.[0]}
            value={instruments}
            onChange={setInstruments}
          />
        ) : (
          <fieldset className="exchange-picker">
            <legend>{t("交易所")}</legend>
            <div className="exchange-choices">
              {offeredExchanges.map(item => (
                <label key={item} className="exchange-choice">
                  <input
                    type="checkbox"
                    checked={chosenExchanges.includes(item)}
                    onChange={event =>
                      setExchanges(
                        event.target.checked
                          ? offeredExchanges.filter(
                              entry => entry === item || chosenExchanges.includes(entry),
                            )
                          : chosenExchanges.filter(entry => entry !== item),
                      )
                    }
                  />
                  <span>{item}</span>
                </label>
              ))}
              {offeredExchanges.length > 1 && (
                <button
                  type="button"
                  className="ghost-action"
                  disabled={chosenExchanges.length === offeredExchanges.length}
                  onClick={() => setExchanges(offeredExchanges)}
                >
                  {t("全选")}
                </button>
              )}
            </div>
          </fieldset>
        )}
        <div className="data-fields">
          <label>
            {t("开始日期")}
            <DateInput value={begin} onChange={setBegin} />
          </label>
          <label>
            {t("结束日期")}
            <DateInput value={end} onChange={setEnd} />
          </label>
        </div>
        {minutes && <p className="subtle data-dialog-hint">{t("日期留空即整个上市期")}</p>}
        <label className="data-checkbox-line">
          <input
            type="checkbox"
            checked={replace}
            onChange={event => setReplace(event.target.checked)}
          />
          {t("重新采集已有分区")}
        </label>
        <ActionError error={failure.error} namespace="asterion.terminal.data-workbench" />
        <div className="data-actions">
          <button type="button" onClick={close}>
            {t("取消")}
          </button>
          <button type="submit" className="primary" disabled={disabled}>
            {t("开始采集")}
          </button>
        </div>
      </form>
    </Dialog>
  );
}
