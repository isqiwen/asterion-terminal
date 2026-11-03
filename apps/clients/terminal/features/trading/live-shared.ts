import type { MessageInitShape } from "@bufbuild/protobuf";
import { decimal } from "../../src/bridge/decimal";
import type { TradingFields, TradingOperation } from "../../src/bridge/trading";
import type { ContractSchema } from "../../src/generated/asterion/domain_pb";
import type { CatalogContract } from "../../src/generated/asterion/market_pb";
import {
  Offset,
  TradingEnvironment,
  type CommandSchema,
  type LiveSnapshot,
  type RiskLimitsSchema,
} from "../../src/generated/asterion/trading_pb";
import { BackendError } from "../../src/i18n/errors";
import { translate, type MessageValues } from "../contract";

export const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);

export const phases: Record<string, string> = {
  disconnected: "未连接",
  connecting: "连接前置",
  authenticating: "终端认证",
  logging_in: "登录中",
  confirming: "确认结算单",
  synchronizing: "同步账户",
  ready: "已就绪",
  error: "连接失败",
};
export const statuses: Record<string, string> = {
  submitted: "已报送",
  accepted: "已接受",
  partially_filled: "部分成交",
  filled: "已成交",
  cancelled: "已撤单",
  rejected: "已拒绝",
};
export const offsets: Record<Offset, string> = {
  [Offset.OFFSET_UNSPECIFIED]: "",
  [Offset.OPEN]: "开仓",
  [Offset.CLOSE_TODAY]: "平今",
  [Offset.CLOSE_YESTERDAY]: "平昨",
  [Offset.CLOSE]: "平仓",
};
export const environmentName = (environment: TradingEnvironment | undefined) =>
  environment === TradingEnvironment.REAL ? "真实资金" : "CTP 仿真";
export const key = (item: { venue: string; symbol: string }) => `${item.venue}.${item.symbol}`;
export const listedKey = (row: CatalogContract) => key(row.instrument!);

// One account request, built from the owner's input when sent; resolves false
// when the input or the request was refused and the panel shows why.
export type Run = (
  operation: TradingOperation | (() => TradingOperation),
  fields?: TradingFields,
) => Promise<boolean>;
export type CommandOperation = MessageInitShape<typeof CommandSchema>["operation"];
// A trading command names the account record and the policy the owner reviewed.
export const command = (run: Run, live: LiveSnapshot, operation: () => CommandOperation) =>
  run(
    () => ({ case: "command", value: { requestId: crypto.randomUUID(), operation: operation() } }),
    { accountId: live.accountId, policyRevision: live.policyRevision },
  );

// Exchange terms of an allowed contract from the CTP contract catalog the
// market service loaded; domestic CTP futures trade whole lots in CNY.
export function policyContract(row: CatalogContract): MessageInitShape<typeof ContractSchema> {
  const [, product, deliveryMonth] = row.contractId.split("/");
  return {
    venue: row.instrument!.venue,
    symbol: row.instrument!.symbol,
    currency: "CNY",
    priceIncrement: decimal(row.priceTick),
    quantityIncrement: decimal("1"),
    multiplier: decimal(String(row.multiplier)),
    product,
    deliveryMonth,
  };
}

export type LimitsDraft = {
  max_order_quantity: string;
  max_gross_quantity: string;
  max_working_orders: string;
  max_price_deviation: string;
};
export function riskLimits(draft: LimitsDraft): MessageInitShape<typeof RiskLimitsSchema> {
  const orders = draft.max_working_orders.trim();
  if (!/^\d+$/.test(orders))
    throw new BackendError("invalid_request", `invalid working order limit: ${orders}`);
  return {
    maxOrderQuantity: decimal(draft.max_order_quantity),
    maxGrossQuantity: decimal(draft.max_gross_quantity),
    maxWorkingOrders: BigInt(orders),
  };
}
