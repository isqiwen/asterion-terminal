import type { RequestClient } from "../../api/requests";
import { useEffect, useId, useState } from "react";

type Reference = {
  id: string;
  manifest: { source: string; scope: Record<string, unknown> };
};
export function CoverageReferences({
  api,
  calendar,
  contracts,
  note,
  symbol,
  disabled,
  onChange,
}: {
  api: RequestClient;
  calendar: string;
  contracts: string;
  note: string;
  symbol: string;
  disabled: boolean;
  onChange: (
    calendar: string,
    contracts: string,
    note: string,
    symbol: string,
  ) => void;
}) {
  const id = useId();
  const [items, setItems] = useState<Record<string, Reference[]>>({});
  const [error, setError] = useState("");
  useEffect(() => {
    let live = true;
    Promise.all(
      ["calendar", "contracts"].map(async (kind) => {
        const result = await api.request<{ items: Reference[] }>(
          `/data/catalog?type_id=futures.${kind}&layer=STANDARD&limit=100`,
        );
        return [kind, result.items || []] as const;
      }),
    )
      .then((values) => {
        if (live) setItems(Object.fromEntries(values));
      })
      .catch((e) => {
        if (live) setError(String(e));
      });
    return () => {
      live = false;
    };
  }, [api]);
  return (
    <fieldset disabled={disabled}>
      <legend>关联外部核对依据</legend>
      <p>
        选择已发布的同交易所日历、合约资料版本；也可粘贴历史版本 ID。候选列出前
        100 个数据集的最新版本。
      </p>
      {(["calendar", "contracts"] as const).map((kind) => (
        <label key={kind}>
          {kind === "calendar" ? "关联日历版本" : "关联合约资料版本"}
          <input
            aria-label={
              kind === "calendar" ? "关联日历版本" : "关联合约资料版本"
            }
            list={`${id}-${kind}`}
            value={kind === "calendar" ? calendar : contracts}
            onChange={(e) =>
              onChange(
                kind === "calendar" ? e.target.value : calendar,
                kind === "contracts" ? e.target.value : contracts,
                note,
                symbol,
              )
            }
          />
          <datalist id={`${id}-${kind}`}>
            {(items[kind] || []).map((v) => (
              <option key={v.id} value={v.id}>
                {v.manifest.source} · {String(v.manifest.scope.exchange || "")}{" "}
                · {v.id}
              </option>
            ))}
          </datalist>
        </label>
      ))}
      <label>
        来源合约代码
        <input
          value={symbol}
          placeholder="填写所选资料的原始代码，例如 RB2505.SHF"
          onChange={(e) => onChange(calendar, contracts, note, e.target.value)}
        />
      </label>
      <label>
        关联说明
        <input
          value={note}
          maxLength={500}
          placeholder="说明为何此日历与合约资料适用于导入行情"
          onChange={(e) =>
            onChange(calendar, contracts, e.target.value, symbol)
          }
        />
      </label>
      {error && <p role="alert">无法读取候选依据，可粘贴版本 ID。{error}</p>}
      <p>关联只用于核对，不改变文件来源；缺口需修正文件并导入新版本。</p>
    </fieldset>
  );
}
