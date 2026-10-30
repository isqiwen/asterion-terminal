import type { AccountRun } from "../../src/bridge/commands";
import type { LiveSession } from "../../src/bridge/client";
import { translate, type MessageValues } from "../contract";

export const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.trading", key, values);

export const phases: Record<LiveSession["phase"], string> = {
  disconnected: "未连接",
  connecting: "连接前置",
  authenticating: "终端认证",
  logging_in: "登录中",
  confirming: "确认结算单",
  synchronizing: "同步账户",
  ready: "已就绪",
  error: "连接失败",
};
export const statuses: Record<LiveSession["orders"][number]["status"], string> = {
  submitted: "已报送",
  accepted: "已接受",
  partially_filled: "部分成交",
  filled: "已成交",
  cancelled: "已撤单",
  rejected: "已拒绝",
};
export const offsets: Record<string, string> = {
  open: "开仓",
  close_today: "平今",
  close_yesterday: "平昨",
  close: "平仓",
};
export const key = (item: { venue: string; symbol: string }) => `${item.venue}.${item.symbol}`;
export type Run = AccountRun;
