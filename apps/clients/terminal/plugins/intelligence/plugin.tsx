import { lazy } from "react";
import { translate, type TerminalPlugin } from "../contract";
import zh from "./locales/zh-CN.json";
import en from "./locales/en-US.json";
const t = (key: string) => translate("asterion.terminal.intelligence", key);
const Panel = lazy(() => import("./NoticeReview").then(m => ({ default: m.NoticeReview })));
const Sets = lazy(() => import("./NoticeSets").then(m => ({ default: m.NoticeSets })));
const Subscriptions = lazy(() =>
  import("./NoticeSubscriptions").then(m => ({ default: m.NoticeSubscriptions })),
);
export const plugin: TerminalPlugin = {
  id: "asterion.terminal.intelligence",
  commands: [
    "node.intelligence.local.open",
    "intelligence.submit",
    "intelligence.get",
    "intelligence.list",
    "intelligence.consumers",
    "intelligence.event.view",
    "intelligence.change",
    "intelligence.set.preview",
    "intelligence.set.publish",
    "intelligence.set.get",
    "intelligence.subscription.preview",
    "intelligence.subscription.create",
    "intelligence.subscription.get",
    "intelligence.subscription.list",
    "intelligence.subscription.control",
    "intelligence.subscription.offer",
    "intelligence.subscription.batch",
    "intelligence.subscription.ack",
    "intelligence.subscription.reminders",
    "intelligence.source.inspect",
    "intelligence.review",
    "intelligence.publish",
    "intelligence.abandon",
    "data.material.read",
    "data.tables.coverage",
    "data.tables.products",
    "data.tables.listed_contracts",
  ],
  languageResources: { "zh-CN": zh, "en-US": en },
  tasks: context =>
    (context.snapshot?.intelligence_service?.records ?? []).map(r => ({
      id: "intelligence:" + r.reference.node + ":" + r.reference.service + ":" + r.reference.id,
      title: r.reference.id + " · " + r.contract,
      status: t(
        {
          preparing: "准备中",
          draft: "待人工核验",
          verified: "已核验待发布",
          publishing: "待保存确认",
          published: "已发布",
          rejected: "已拒绝",
          abandoned: "已放弃",
        }[r.state],
      ),
      completed: r.state === "published" ? 1 : 0,
      total: 1,
      open: () =>
        context.navigate("workspace.intelligence-notices", { params: { id: r.reference.id } }),
    })),
  workspaces: [
    {
      id: "workspace.intelligence-subscriptions",
      get title() {
        return t("情报订阅与交付");
      },
      component: context => <Subscriptions context={context} />,
    },
    {
      id: "workspace.intelligence-sets",
      get title() {
        return t("固定事件集合");
      },
      component: context => <Sets context={context} />,
    },
    {
      id: "workspace.intelligence-notices",
      get title() {
        return t("人工公告核验");
      },
      component: context => <Panel context={context} />,
    },
  ],
};
