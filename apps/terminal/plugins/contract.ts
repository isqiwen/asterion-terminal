import type { DisplayError } from "../src/i18n/errors";
export { ErrorNotice, asDisplayError } from "../src/i18n/errors";
export type { DisplayError } from "../src/i18n/errors";
import type { LanguageResources } from "../src/i18n";
export { translate, getLocale } from "../src/i18n";
export type { MessageValues, LanguageResources } from "../src/i18n";
import type { CsvRequest, Snapshot, TerminalCommand } from "../src/bridge/client";
import type { ComponentType, ReactNode } from "react";

// Terminal 插件接口定义：宿主提供的能力与插件贡献的工作区、卡片。
// 契约不依赖任何具体插件；总览插件只是 DashboardWidget 的一个消费者。

// Context handed to overview cards when they render.
export type DashboardContext = Readonly<{
  widgets: DashboardWidget[];
  status?: ReactNode;
  storageKey: string;
  refresh: () => void;
  catalogError: DisplayError;
  openTasks: () => void;
}>;
export type DashboardWidget = {
  id: string;
  title: string;
  category: string;
  description: string;
  width: 1 | 2;
  defaultVisible?: boolean;
  hasContent?: boolean;
  column?: "primary" | "secondary";
  position?: "main" | "aside";
  attentionCount?: (context: DashboardContext) => number;
  render: (context: DashboardContext) => ReactNode;
};
export type TerminalContext = {
  snapshot: Snapshot | null;
  busy: boolean;
  error: DisplayError;
  widgets: DashboardWidget[];
  marketMode?: "live" | "history";
  navigate: (id: string, options?: { marketMode: "live" | "history" }) => void;
  openSettings: (page?: "appearance" | "connections") => void;
  openTasks: () => void;
  refresh: () => void;
  trade: (method: TerminalCommand, params?: Record<string, unknown>) => Promise<void>;
  inspect: (params: CsvRequest) => Promise<void>;
};
export type TerminalPlugin = {
  id: string;
  apiVersion: 1;
  languageResources?: LanguageResources;
  workspace: {
    id: string;
    title: string;
    icon: string;
    section: string;
    component: ComponentType<TerminalContext>;
  };
  tasks?: (context: TerminalContext) => TerminalTask[];
  widgets?: (context: TerminalContext) => DashboardWidget[];
};

export type TerminalTask = {
  id: string;
  title: string;
  status: string;
  completed: number;
  total: number;
  open: () => void;
};
