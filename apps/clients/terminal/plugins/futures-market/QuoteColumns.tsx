import { useEffect, useRef, useState } from "react";
import { translate, type MessageValues } from "../contract";
const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.futures-market", key, values);

export const quoteColumns = ["最新价", "涨跌幅", "涨跌", "1分钟涨速", "持仓量", "成交量"] as const;
export type QuoteColumn = (typeof quoteColumns)[number];
export type QuoteColumnSetting = { column: QuoteColumn; visible: boolean };
export const defaultQuoteColumns = (): QuoteColumnSetting[] =>
  quoteColumns.map(column => ({ column, visible: column !== "成交量" }));

export function QuoteColumns({
  value,
  apply,
  close,
}: {
  value: QuoteColumnSetting[];
  apply: (value: QuoteColumnSetting[]) => void;
  close: () => void;
}) {
  const [draft, setDraft] = useState(value);
  const dialog = useRef<HTMLDialogElement>(null);
  const dragged = useRef<QuoteColumn | null>(null);
  useEffect(() => {
    const element = dialog.current;
    const previous = document.activeElement;
    element?.showModal();
    return () => {
      element?.close();
      if (previous instanceof HTMLElement && previous.isConnected) previous.focus();
    };
  }, []);
  function move(column: QuoteColumn, target: QuoteColumn) {
    setDraft(previous => {
      const from = previous.findIndex(item => item.column === column);
      const to = previous.findIndex(item => item.column === target);
      if (from < 0 || to < 0 || from === to) return previous;
      const next = [...previous];
      next.splice(to, 0, ...next.splice(from, 1));
      return next;
    });
  }
  return (
    <dialog
      ref={dialog}
      className="quote-columns-dialog"
      aria-label={t("行情表头设置")}
      onCancel={event => {
        event.preventDefault();
        close();
      }}
    >
      <h2>{t("行情表头设置")}</h2>
      <p>{t("合约列固定；拖动或使用上下按钮调整报价列。设置在当前窗口保留。")}</p>
      <ul>
        {draft.map((item, index) => (
          <li
            key={item.column}
            draggable
            onDragStart={event => {
              dragged.current = item.column;
              event.dataTransfer.effectAllowed = "move";
              event.dataTransfer.setData("text/plain", item.column);
            }}
            onDragEnd={() => {
              dragged.current = null;
            }}
            onDragOver={event => {
              if (dragged.current) event.preventDefault();
            }}
            onDrop={event => {
              event.preventDefault();
              if (dragged.current) move(dragged.current, item.column);
              dragged.current = null;
            }}
          >
            <label>
              <input
                type="checkbox"
                checked={item.visible}
                onChange={event => {
                  const visible = event.target.checked;
                  setDraft(previous =>
                    previous.map(value =>
                      value.column === item.column ? { ...value, visible } : value,
                    ),
                  );
                }}
              />
              {t(item.column)}
            </label>
            <button
              type="button"
              aria-label={t("上移 {column}", { column: t(item.column) })}
              disabled={index === 0}
              onClick={() => move(item.column, draft[index - 1].column)}
            >
              ↑
            </button>
            <button
              type="button"
              aria-label={t("下移 {column}", { column: t(item.column) })}
              disabled={index === draft.length - 1}
              onClick={() => move(item.column, draft[index + 1].column)}
            >
              ↓
            </button>
          </li>
        ))}
      </ul>
      <footer>
        <button type="button" onClick={() => setDraft(defaultQuoteColumns())}>
          {t("恢复默认列")}
        </button>
        <button type="button" onClick={close}>
          {t("取消列设置")}
        </button>
        <button
          type="button"
          onClick={() => {
            apply(draft);
            close();
          }}
        >
          {t("应用列设置")}
        </button>
      </footer>
    </dialog>
  );
}
