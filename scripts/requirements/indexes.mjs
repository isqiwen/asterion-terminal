import fs from "node:fs/promises";
import path from "node:path";

const decisionKinds = new Set(["规格决定", "运行配置", "交付准入"]);

// Discover definitions before validating their schema. Matching only valid kinds
// would silently hide a decision when an author introduces an unknown kind.
export function decisionDefinitions(text, file) {
  const result = [];
  for (const match of text.matchAll(/^- \*\*(DEC-[^*\n]+)\*\*([^\n]*)$/gm)) {
    const [line, id, fields] = match;
    if (!/^DEC-[A-Z0-9-]+$/.test(id))
      throw Error(`Invalid decision identity ${id}: ${file}`);
    const kind = fields.match(/^[:：]\s*\*\*([^*]+)\*\*/)?.[1];
    if (!decisionKinds.has(kind))
      throw Error(
        `Unknown decision kind ${kind || "(missing)"}: ${id} in ${file}`,
      );
    if (!/^：\*\*[^*]+\*\*。\S[^\n]*$/.test(fields))
      throw Error(`Malformed decision definition ${id}: ${file}: ${line}`);
    result.push({ id, kind, file });
  }
  return result;
}

// Read-only projections of maintained requirements. Never write module content.
export async function generateIndexes(root) {
  const sources = [];
  async function walk(dir) {
    for (const entry of await fs.readdir(dir, { withFileTypes: true })) {
      const file = path.join(dir, entry.name);
      if (entry.isDirectory()) await walk(file);
      else if (
        file.endsWith(".md") &&
        !["decisions.md", "traceability.md"].includes(entry.name)
      )
        sources.push([
          path.relative(root, file).split(path.sep).join("/"),
          await fs.readFile(file, "utf8"),
        ]);
    }
  }
  await walk(root);
  const order = ["DAT", "INT", "ANA", "STR", "BKT", "EVA", "LIV", "SYS", "COM"];
  const names = [
    "数据",
    "情报",
    "分析",
    "策略",
    "回测",
    "评估",
    "实盘",
    "系统管理",
    "通用规范",
  ];
  const decisions = [],
    features = [],
    journeys = [];
  for (const [file, text] of sources) {
    decisions.push(...decisionDefinitions(text, file));
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
      journeys.push({
        id: m[1],
        file,
        ui: new Set(m[3].match(/UI-[A-Z]+-[A-Z]+-\d+/g) || []),
        features: new Set(
          m[3].match(/FR-[A-Z]+-[A-Z]+-\d+-\d+(?![\d-])/g) || [],
        ),
      });
  }
  const decisionIds = new Set();
  for (const decision of decisions) {
    if (decisionIds.has(decision.id))
      throw Error(`Duplicate decision source ${decision.id}`);
    if (!order.includes(decision.id.split("-")[1]))
      throw Error(`Unknown decision module ${decision.id}: ${decision.file}`);
    decisionIds.add(decision.id);
  }
  const ref = (id, file, label = id) =>
    `[${label}](${file}#${id.toLowerCase()})`;
  let d =
    "# 决定、运行配置与交付准入登记\n\n[需求总纲](README.md) · [交付与验收](delivery.md)\n\n本文由所属需求的DEC记录生成，禁止直接修改索引。详细结论在对应正文唯一维护。**规格决定**定义开发采用的设计约束；**运行配置**由使用者按字段约束填写；**交付准入**规定接入真实环境所需的外部证据与核验条件。\n";
  let t =
    "# 全量功能追踪与验收定位\n\n[需求总纲](README.md) · [交付与验收](delivery.md)\n\n此表从正文派生，不能在这里修改功能。每行保留一个FR及其主题、UI、AC数量、正文明确引用该FR的旅程和共用界面流程。明确关联表示旅程引用了该功能；共用界面仅由UI交集生成，用于导航，不能推断行为覆盖。两者都不能替代逐AC的步骤、断言、预期结果和运行证据。跨模块共用功能不得因多个页面引用而重复维护。\n\n主题/功能/界面编号均可点击查看验收条文。开发时按交付说明将每条AC映射到验证用例及运行证据；缺少明确旅程引用的功能仍须独立完成验收。\n";
  const uiLocations = new Map();
  for (const [file, text] of sources)
    for (const m of text.matchAll(/^## (UI-[A-Z0-9-]+) /gm))
      uiLocations.set(m[1], file);
  for (let i = 0; i < order.length; i++) {
    d += `\n## ${names[i]}\n\n| 决定 | 类型 | 唯一结论位置 |\n| --- | --- | --- |\n`;
    for (const x of decisions
      .filter((x) => x.id.startsWith(`DEC-${order[i]}-`))
      .sort((a, b) => a.id.localeCompare(b.id)))
      d += `| ${x.id} | ${x.kind} | ${ref(x.id, x.file, "结论、约束与准入条件")} |\n`;
    t += `\n## ${names[i]}\n\n| 功能 | 所属主题 | 页面 | 功能AC数 | 明确关联旅程 | 共用界面流程（仅导航） |\n| --- | --- | --- | --- | --- | --- |\n`;
    for (const x of features
      .filter((x) => x.id.startsWith(`FR-${order[i]}-`))
      .sort((a, b) => a.id.localeCompare(b.id))) {
      const req = x.id.replace(/^FR-/, "REQ-").replace(/-\d+$/, "");
      const related = journeys.filter((j) => x.ui.some((ui) => j.ui.has(ui)));
      const explicit = journeys.filter((j) => j.features.has(x.id));
      t += `| ${ref(x.id, x.file, x.id + " " + x.title)} | ${ref(req, x.file)} | ${x.ui.map((ui) => ref(ui, uiLocations.get(ui))).join("；")} | ${x.ac.length} | ${explicit.map((j) => ref(j.id, j.file)).join("；") || "独立功能验收"} | ${related.map((j) => ref(j.id, j.file)).join("；") || "无共用界面流程"} |\n`;
    }
  }
  t += "\n## 非功能验收\n\n";
  for (const [file, text] of sources)
    for (const m of text.matchAll(/^## (NFR-[A-Z0-9-]+) ([^\n]+)/gm))
      t += `- ${ref(m[1], file, m[1] + " " + m[2])}：按本条环境、阈值、测量、失败规则和验收执行要求独立验证。\n`;
  await fs.writeFile(path.join(root, "decisions.md"), d);
  await fs.writeFile(path.join(root, "traceability.md"), t);
  return { decisions: decisions.length, features: features.length };
}
