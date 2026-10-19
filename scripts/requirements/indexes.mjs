import fs from "node:fs/promises";
import path from "node:path";

// Read-only projections of maintained requirements. Never write module content.
export async function generateIndexes(root) {
  const sources = [];
  async function walk(dir) {
    for (const entry of await fs.readdir(dir, { withFileTypes: true })) {
      const file = path.join(dir, entry.name);
      if (entry.isDirectory()) await walk(file);
      else if (file.endsWith(".md") && !["decisions.md", "traceability.md"].includes(entry.name))
        sources.push([
          path.relative(root, file).split(path.sep).join("/"),
          await fs.readFile(file, "utf8"),
        ]);
    }
  }
  await walk(root);
  const order = ["DAT", "INT", "ANA", "STR", "BKT", "EVA", "LIV", "SYS", "COM"];
  const names = ["数据", "情报", "分析", "策略", "回测", "评估", "实盘", "系统管理", "通用规范"];
  const decisions = [],
    features = [],
    journeys = [];
  for (const [file, text] of sources) {
    for (const m of text.matchAll(/^- \*\*(DEC-[A-Z0-9-]+)\*\*：\*\*([^*]+)\*\*。([^\n]+)/gm))
      decisions.push({ id: m[1], kind: m[2], file });
    for (const m of text.matchAll(
      /^### (FR-[A-Z0-9-]+) ([^\n]+)\n([\s\S]*?)(?=^<a id=|(?![\s\S]))/gm,
    )) {
      const uiLine = m[3].match(/\*\*界面对应：\*\*([^\n]+)/)?.[1] || "";
      features.push({
        id: m[1],
        title: m[2],
        file,
        ui: [...new Set(uiLine.match(/UI-[A-Z]+-[A-Z]+-\d+/g) || [])],
        ac: [...new Set(m[3].match(new RegExp(m[1] + "-AC\\d+", "g")) || [])],
      });
    }
    for (const m of text.matchAll(
      /^## (JRN-\d+) ([^\n]+)\n([\s\S]*?)(?=^<a id="jrn-|(?![\s\S]))/gm,
    ))
      journeys.push({ id: m[1], file, ui: new Set(m[3].match(/UI-[A-Z]+-[A-Z]+-\d+/g) || []) });
  }
  const ref = (id, file, label = id) => `[${label}](${file}#${id.toLowerCase()})`;
  let d =
    "# 决定、运行配置与交付准入登记\n\n[需求总纲](README.md) · [交付与验收](delivery.md)\n\n本文由所属需求的DEC记录生成，禁止直接修改索引。原88项问题均有明确处理；详细结论在对应正文唯一维护。**规格决定**表示本次编制采用的设计，不冒称用户逐项签认；**运行配置**由使用者按字段约束填写；**交付准入**要求真实外部证据，未核验不宣称可用。\n";
  let t =
    "# 全量功能追踪与验收定位\n\n[需求总纲](README.md) · [交付与验收](delivery.md)\n\n此表从正文派生，不能在这里修改功能。每行保留一个FR及其主题、UI、AC数量和关联旅程。旅程由UI交集推导，只表示可从哪些流程检查该功能，**不是已执行覆盖率**；验收必须逐AC建立实际证据，当前全部产品验收状态为未执行。跨模块共用功能不得因多个页面引用而重复维护。\n\n主题/功能/界面编号均可点击查看实际验收条文；运行结果按交付说明登记，不用文档链接替代测试结果。\n";
  const uiLocations = new Map();
  for (const [file, text] of sources)
    for (const m of text.matchAll(/^## (UI-[A-Z0-9-]+) /gm)) uiLocations.set(m[1], file);
  for (let i = 0; i < order.length; i++) {
    d += `\n## ${names[i]}\n\n| 决定 | 类型 | 唯一结论位置 |\n| --- | --- | --- |\n`;
    for (const x of decisions
      .filter(x => x.id.startsWith(`DEC-${order[i]}-`))
      .sort((a, b) => a.id.localeCompare(b.id)))
      d += `| ${x.id} | ${x.kind} | ${ref(x.id, x.file, "结论、约束与准入条件")} |\n`;
    t += `\n## ${names[i]}\n\n| 功能 | 所属主题 | 页面 | 功能AC数 | 关联旅程（非已测覆盖） |\n| --- | --- | --- | --- | --- |\n`;
    for (const x of features
      .filter(x => x.id.startsWith(`FR-${order[i]}-`))
      .sort((a, b) => a.id.localeCompare(b.id))) {
      const req = x.id.replace(/^FR-/, "REQ-").replace(/-\d+$/, "");
      const related = journeys.filter(j => x.ui.some(ui => j.ui.has(ui)));
      t += `| ${ref(x.id, x.file, x.id + " " + x.title)} | ${ref(req, x.file)} | ${x.ui.map(ui => ref(ui, uiLocations.get(ui))).join("；")} | ${x.ac.length} | ${related.map(j => ref(j.id, j.file)).join("；") || "独立功能验收"} |\n`;
    }
  }
  t += "\n## 非功能验收\n\n";
  for (const [file, text] of sources)
    for (const m of text.matchAll(/^## (NFR-[A-Z0-9-]+) ([^\n]+)/gm))
      t += `- ${ref(m[1], file, m[1] + " " + m[2])}：按本条环境、阈值、测量和失败规则独立验收，当前未执行。\n`;
  await fs.writeFile(path.join(root, "decisions.md"), d);
  await fs.writeFile(path.join(root, "traceability.md"), t);
  return { decisions: decisions.length, features: features.length };
}
