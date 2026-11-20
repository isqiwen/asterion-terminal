import type { MessageInitShape } from "@bufbuild/protobuf";
import { useEffect, useState } from "react";
import { decimal } from "../../src/bridge/decimal";
import { tradingRequest, type TradingCall } from "../../src/bridge/trading";
import { Offset } from "../../src/generated/asterion/common_pb";
import type { RiskLimitsSchema } from "../../src/generated/asterion/common_pb";
import { CatalogState, type CatalogContract } from "../../src/generated/asterion/market_pb";
import {
  BrokerState,
  Environment,
  OrderState,
  type Account,
  type CommandSchema,
  type ContractTerms,
  type ContractTermsSchema,
  type ListedContract,
} from "../../src/generated/asterion/trading_pb";
import { useMarket } from "../../src/host/market";
import { BackendError } from "../../src/i18n/errors";
import { type MessageValues, translate } from "../../src/i18n";

export const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);

export const phases: Partial<Record<BrokerState, string>> = {
  [BrokerState.UNSPECIFIED]: "未连接",
  [BrokerState.DISCONNECTED]: "未连接",
  [BrokerState.CONNECTING]: "连接前置",
  [BrokerState.AUTHENTICATING]: "终端认证",
  [BrokerState.LOGGING_IN]: "登录中",
  [BrokerState.CONFIRMING]: "确认结算单",
  [BrokerState.SYNCHRONIZING]: "同步账户",
  [BrokerState.READY]: "已就绪",
  [BrokerState.FAILED]: "连接失败",
};
export const statuses: Partial<Record<OrderState, string>> = {
  [OrderState.SUBMITTED]: "已报送",
  [OrderState.ACCEPTED]: "已接受",
  [OrderState.PARTIALLY_FILLED]: "部分成交",
  [OrderState.FILLED]: "已成交",
  [OrderState.CANCELLED]: "已撤单",
  [OrderState.REJECTED]: "已拒绝",
};
export const offsets: Partial<Record<Offset, string>> = {
  [Offset.OPEN]: "开仓",
  [Offset.CLOSE_TODAY]: "平今",
  [Offset.CLOSE_YESTERDAY]: "平昨",
  [Offset.CLOSE]: "平仓",
};
// The counter's funds as the page and an exported day show them.
export const fundFields = [
  ["balance", "动态权益"],
  ["available", "可用资金"],
  ["margin", "占用保证金"],
  ["positionProfit", "持仓盈亏"],
  ["closeProfit", "平仓盈亏"],
  ["commission", "手续费"],
  ["dynamicEquity", "盯市净值"],
  ["preBalance", "上次结算准备金"],
  ["deposit", "入金"],
  ["withdraw", "出金"],
] as const;

export function formatRiskBreach(breach: string) {
  if (breach === "daily_loss") return t("日内亏损达限");
  if (breach === "drawdown") return t("最大回撤达限");
  if (breach === "margin_ratio") return t("保证金率达限");
  if (breach.startsWith("daily_cancels:"))
    return t("{contract} 日撤单达限", { contract: breach.slice("daily_cancels:".length) });
  return breach;
}
export const environmentName = (environment: Environment | undefined) =>
  environment === Environment.REAL ? "真实资金" : "CTP 仿真";
export const key = (item: { exchange: string; symbol: string } | undefined) =>
  `${item?.exchange}.${item?.symbol}`;
export type CatalogEntry = CatalogContract | ListedContract;
export const listedKey = (row: CatalogEntry) => key(row.instrument);

export function useTradingCatalog(connection: string) {
  const market = useMarket()?.board.catalog;
  const marketReady = market?.status?.state === CatalogState.READY;
  const [state, setState] = useState<{
    connection: string;
    contracts: ListedContract[];
    ready: boolean;
  }>({ connection: "", contracts: [], ready: false });
  useEffect(() => {
    if (marketReady || !connection) return;
    let active = true;
    void tradingRequest(connection, { case: "getCatalog", value: {} })
      .then(reply => {
        if (active && reply.result.case === "getCatalog")
          setState({
            connection,
            contracts: reply.result.value.contracts,
            ready: reply.result.value.contracts.length > 0,
          });
      })
      .catch(() => {
        if (active) setState({ connection, contracts: [], ready: false });
      });
    return () => {
      active = false;
    };
  }, [connection, marketReady]);
  if (marketReady) return { contracts: market.contracts as CatalogEntry[], ready: true };
  if (state.connection === connection && state.ready)
    return { contracts: state.contracts as CatalogEntry[], ready: true };
  return { contracts: [] as CatalogEntry[], ready: false };
}

// One account request, built from the owner's input when sent; resolves false
// when the input or the request was refused and the panel shows why.
export type Run = (operation: TradingCall | (() => TradingCall)) => Promise<boolean>;
export type CommandAction = MessageInitShape<typeof CommandSchema>["action"];
// A trading command names the account and the policy revision the owner reviewed.
export const command = (run: Run, account: Account, action: () => CommandAction) =>
  run(() => ({
    case: "sendCommand",
    value: {
      requestId: crypto.randomUUID(),
      accountId: account.id,
      policyRevision: account.session?.policyRevision ?? "",
      action: action(),
    },
  }));

export function isProductPolicy(item: {
  instrument?: { symbol: string };
  product: string;
  deliveryMonth: string;
}) {
  return !item.deliveryMonth && Boolean(item.product) && item.instrument?.symbol === item.product;
}

export function findPolicyTerms(
  contracts: readonly ContractTerms[] | undefined,
  instrument: { exchange: string; symbol: string } | undefined,
): ContractTerms | undefined {
  if (!contracts?.length || !instrument) return undefined;
  const exact = contracts.find(
    item =>
      item.instrument?.exchange === instrument.exchange &&
      item.instrument.symbol === instrument.symbol,
  );
  if (exact) return exact;
  return contracts.find(
    item =>
      isProductPolicy(item) &&
      item.instrument?.exchange === instrument.exchange &&
      instrument.symbol.toLowerCase().startsWith(item.product.toLowerCase()) &&
      /^\d{3,4}$/.test(instrument.symbol.slice(item.product.length)),
  );
}

export function rowProduct(row: CatalogEntry) {
  return row.product || row.instrument?.symbol.replace(/\d+$/, "") || row.contract.split("/")[1];
}

// Exchange terms of an allowed contract from the CTP contract catalog the
// market service loaded; domestic CTP futures trade whole lots.
export function policyContract(row: CatalogEntry): MessageInitShape<typeof ContractTermsSchema> {
  const [, , deliveryMonth] = row.contract.split("/");
  return {
    instrument: row.instrument,
    product: rowProduct(row),
    deliveryMonth,
    multiplier: decimal(String(row.multiplier)),
    priceTick: row.priceTick,
    lotSize: decimal("1"),
  };
}

export function productKey(row: CatalogEntry) {
  const exchange = row.instrument?.exchange ?? row.contract.split("/")[0];
  return `${exchange}.${rowProduct(row)}`;
}

export function policyProducts(listed: readonly CatalogEntry[]) {
  const seen = new Set<string>();
  const out: { key: string; sample: CatalogEntry }[] = [];
  for (const row of listed) {
    const id = productKey(row);
    if (!seen.has(id)) {
      seen.add(id);
      out.push({ key: id, sample: row });
    }
  }
  return out;
}

export function policyProductContract(
  row: CatalogEntry,
): MessageInitShape<typeof ContractTermsSchema> {
  const exchange = row.instrument?.exchange ?? row.contract.split("/")[0];
  const product = rowProduct(row);
  return {
    instrument: { exchange, symbol: product },
    product,
    deliveryMonth: "",
    multiplier: decimal(String(row.multiplier)),
    priceTick: row.priceTick,
    lotSize: decimal("1"),
  };
}

export function policyEntryFromCatalog(
  value: string,
  listed: readonly CatalogEntry[],
): MessageInitShape<typeof ContractTermsSchema> | undefined {
  const exact = listed.find(item => listedKey(item) === value);
  if (exact) return policyContract(exact);
  const byProduct = listed.find(item => productKey(item) === value);
  if (byProduct) return policyProductContract(byProduct);
  return undefined;
}

export type LimitsDraft = {
  max_order_quantity: string;
  max_gross_quantity: string;
  max_contract_quantity: string;
  max_working_orders: string;
  max_price_deviation: string;
  max_daily_loss: string;
  max_drawdown: string;
  max_margin_ratio: string;
  max_daily_cancels_per_contract: string;
};
// Each limit's field, label and unit. Ratios are entered as percentages.
export const requiredLimitFields = [
  ["max_order_quantity", "单笔数量上限", "手"],
  ["max_contract_quantity", "单合约持仓上限", "手"],
  ["max_gross_quantity", "账户总持仓上限", "手"],
  ["max_working_orders", "在途委托数上限", "笔"],
  ["max_price_deviation", "价格偏离上限", "%"],
] as const;
export const optionalLimitFields = [
  ["max_daily_loss", "日内亏损限额", "元"],
  ["max_drawdown", "最大回撤限额", "元"],
  ["max_margin_ratio", "保证金率上限", "%"],
  ["max_daily_cancels_per_contract", "单合约日撤单上限", "次"],
] as const;
function canonical(whole: string, fraction: string) {
  const digits = whole.replace(/^0+(?=\d)/, "");
  const rest = fraction.replace(/0+$/, "");
  return rest ? `${digits}.${rest}` : digits;
}
// A percentage as the ratio Trading records: "2" is 0.02. Other text passes
// through for decimal() to refuse.
export function percentRatio(percent: string) {
  const text = percent.trim();
  if (!/^\d+(\.\d+)?$/.test(text)) return text;
  const [whole, fraction = ""] = text.split(".");
  const digits = whole.padStart(3, "0");
  return canonical(digits.slice(0, -2), digits.slice(-2) + fraction);
}
// A limit with its unit, as a review or summary lists it.
export function limitText(label: string, value: string, unit: string) {
  return unit === "%" ? `${label} ${value}%` : `${label} ${value} ${t(unit)}`;
}
// A recorded ratio as the percentage the form shows: 0.02 is "2".
export function ratioPercent(ratio: string) {
  const [whole, fraction = ""] = ratio.split(".");
  const padded = fraction.padEnd(2, "0");
  return canonical(whole + padded.slice(0, 2), padded.slice(2));
}
export function riskLimits(draft: LimitsDraft): MessageInitShape<typeof RiskLimitsSchema> {
  const orders = draft.max_working_orders.trim();
  const workingOrders = Number(orders);
  if (!/^[1-9]\d*$/.test(orders) || workingOrders > 0xffff_ffff)
    throw new BackendError("invalid_request", `invalid working order limit: ${orders}`);
  return {
    maxOrderLots: decimal(draft.max_order_quantity),
    maxGrossLots: decimal(draft.max_gross_quantity),
    maxContractLots: decimal(draft.max_contract_quantity),
    maxWorkingOrders: workingOrders,
  };
}
export function policyOptionalFields(draft: Partial<LimitsDraft>) {
  const cancels = (draft.max_daily_cancels_per_contract ?? "").trim();
  let maxDailyCancelsPerContract = 0;
  if (cancels) {
    const parsed = Number(cancels);
    if (!/^[1-9]\d*$/.test(cancels) || parsed > 0xffff_ffff)
      throw new BackendError("invalid_request", `invalid daily cancel limit: ${cancels}`);
    maxDailyCancelsPerContract = parsed;
  }
  const loss = (draft.max_daily_loss ?? "").trim();
  const drawdown = (draft.max_drawdown ?? "").trim();
  const marginRatio = (draft.max_margin_ratio ?? "").trim();
  return {
    ...(loss ? { maxDailyLoss: decimal(loss) } : {}),
    ...(drawdown ? { maxDrawdown: decimal(drawdown) } : {}),
    ...(marginRatio ? { maxMarginRatio: decimal(percentRatio(marginRatio)) } : {}),
    ...(maxDailyCancelsPerContract > 0 ? { maxDailyCancelsPerContract } : {}),
  };
}
