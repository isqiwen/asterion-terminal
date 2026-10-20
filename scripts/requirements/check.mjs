import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { decisionDefinitions } from "./indexes.mjs";
const root = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "../../docs/requirements",
);
async function walk(dir) {
  const result = [];
  for (const entry of await fs.readdir(dir, { withFileTypes: true })) {
    const file = path.join(dir, entry.name);
    if (entry.isDirectory()) result.push(...(await walk(file)));
    else if (file.endsWith(".md")) result.push(file);
  }
  return result;
}
const files = await walk(root),
  texts = new Map(
    await Promise.all(
      files.map(async (f) => [f, await fs.readFile(f, "utf8")]),
    ),
  );
const definitions = new Map(),
  anchors = new Map(),
  decisionSources = new Map();
for (const [file, text] of texts) {
  if (!["decisions.md", "traceability.md"].includes(path.basename(file)))
    for (const decision of decisionDefinitions(text, file)) {
      if (decisionSources.has(decision.id))
        throw Error(`Duplicate decision source ${decision.id}`);
      decisionSources.set(decision.id, decision);
    }
  const ids = [...text.matchAll(/<a id="([^"]+)"/g)].map((m) => m[1]);
  if (ids.length !== new Set(ids).size)
    throw Error(`Duplicate anchor: ${file}`);
  anchors.set(file, new Set(ids));
  for (const match of text.matchAll(
    /^#{2,3} ((?:REQ|FR|UI|JRN|NFR)-[A-Z0-9-]+) (.+)$/gm,
  )) {
    if (definitions.has(match[1]))
      throw Error(`Duplicate definition ${match[1]}`);
    definitions.set(match[1], { file, title: match[2] });
    if (!ids.includes(match[1].toLowerCase()))
      throw Error(`Missing anchor ${match[1]}`);
  }
}
for (const [file, text] of texts) {
  for (const m of text.matchAll(/\]\(([^)]+)\)/g)) {
    const href = m[1];
    if (/^(https?:|mailto:)/.test(href)) continue;
    const [name, anchor] = href.split("#");
    const target = name ? path.resolve(path.dirname(file), name) : file;
    await fs.access(target);
    if (target.endsWith(".md") && anchor && !anchors.get(target)?.has(anchor))
      throw Error(`Missing anchor ${file}: ${href}`);
  }
  for (const match of text.matchAll(
    /<a id="fr-[^"]+"><\/a>\s*\n### (FR-[A-Z0-9-]+) [^\n]+\n(.*?)(?=<a id=|$)/gs,
  )) {
    const body = match[2];
    for (const field of [
      "目标与结果",
      "触发",
      "输入与约束",
      "目标操作与反馈",
      "异常与边界",
      "验收条件",
      "界面对应",
    ])
      if (!body.includes(`**${field}：**`))
        throw Error(`Missing ${field}: ${match[1]}`);
    if (!body.includes(`${match[1]}-AC01`))
      throw Error(`Missing acceptance: ${match[1]}`);
  }
}
const all = [...texts.values()].join("\n");
const ac = [
  ...all.matchAll(
    /(?:\*\*|\| )((?:REQ|FR|UI)-[A-Z0-9-]+-AC\d+)(?:\*\*|\s*\|)/g,
  ),
].map((m) => m[1]);
if (ac.length !== new Set(ac).size)
  throw Error("Duplicate acceptance definition");
const counts = Object.fromEntries(
  ["REQ", "FR", "UI", "JRN", "NFR"].map((prefix) => [
    prefix,
    [...definitions.keys()].filter((id) => id.startsWith(prefix + "-")).length,
  ]),
);
for (const [prefix, minimum] of Object.entries({
  REQ: 65,
  FR: 304,
  UI: 58,
  JRN: 42,
  NFR: 10,
}))
  if (counts[prefix] < minimum)
    throw Error(
      `Requirements scope removed: ${prefix} ${counts[prefix]} < ${minimum}`,
    );
for (let n = 1; n <= 42; n++)
  if (!definitions.has(`JRN-${String(n).padStart(3, "0")}`))
    throw Error(`Missing preserved journey JRN-${String(n).padStart(3, "0")}`);
const html = await fs.readFile(path.join(root, "requirements.html"), "utf8");
const prototype = await fs.readFile(path.join(root, "prototype.html"), "utf8");
if (!prototype.includes('name="requirements-view" content="target-ui-design"'))
  throw Error("UI browser is not generated from target design sources");
for (const [file, text] of [
  ...texts,
  ["requirements.html", html],
  ["prototype.html", prototype],
])
  if (/(?:screens\/[^\s)"<>]+\.png|data:image\/png)/.test(text))
    throw Error(
      `Current-runtime screenshot dependency in target requirements: ${file}`,
    );
for (const id of definitions.keys())
  if (!html.includes(`id="${id.toLowerCase()}"`))
    throw Error(`Reader missing ${id}`);
const trace = texts.get(path.join(root, "traceability.md"));
const decisionIndex = texts.get(path.join(root, "decisions.md"));
const indexedDecisions = new Map();
for (const line of decisionIndex.split("\n")) {
  if (!/^\|\s*DEC-/.test(line)) continue;
  const cells = line.split("|").map((cell) => cell.trim());
  const [id, kind, location] = cells.slice(1, 4);
  const source = decisionSources.get(id);
  if (!source) throw Error(`Decision index has no source: ${id}`);
  if (indexedDecisions.has(id)) throw Error(`Duplicate decision index ${id}`);
  if (kind !== source.kind)
    throw Error(`Decision index kind disagrees with source: ${id}`);
  const relative = path.relative(root, source.file).split(path.sep).join("/");
  if (!location.includes(`](${relative}#${id.toLowerCase()})`))
    throw Error(`Decision index location disagrees with source: ${id}`);
  indexedDecisions.set(id, kind);
}
for (const id of decisionSources.keys())
  if (!indexedDecisions.has(id)) throw Error(`Missing decision index ${id}`);
const journeyRefs = new Map();
const journeyText = texts.get(path.join(root, "workflows/README.md"));
for (const m of journeyText.matchAll(
  /^## (JRN-\d+) [^\n]+\n([\s\S]*?)(?=^<a id="jrn-|(?![\s\S]))/gm,
)) {
  for (const field of [
    "**目标：**",
    "**开始前：**",
    "### 正常步骤",
    "### 分支与恢复",
    "### 完成标准",
    "### 保留的资料",
  ])
    if (!m[2].includes(field)) throw Error(`Missing journey ${field}: ${m[1]}`);
  const refs = new Set(m[2].match(/FR-[A-Z]+-[A-Z]+-\d+-\d+(?![\d-])/g) || []);
  for (const id of refs)
    if (!definitions.has(id))
      throw Error(`Unknown explicit feature reference ${id} in ${m[1]}`);
  journeyRefs.set(m[1], refs);
}
let tracedFeatures = 0,
  specifiedPages = 0;
for (const [file, text] of texts) {
  if (["traceability.md", "decisions.md"].includes(path.basename(file)))
    continue;
  for (const m of text.matchAll(
    /^### (FR-[A-Z0-9-]+) [^\n]+\n([\s\S]*?)(?=^<a id=|(?![\s\S]))/gm,
  )) {
    const ui = m[2].match(/\*\*界面对应：\*\*([^\n]+)/)?.[1];
    if (!ui?.includes("UI-")) throw Error(`No UI mapping: ${m[1]}`);
    const acCount = new Set(m[2].match(new RegExp(m[1] + "-AC\\d+", "g")) || [])
      .size;
    const rows = trace
      .split("\n")
      .filter((row) => row.startsWith(`| [${m[1]} `));
    if (!acCount || rows.length !== 1) throw Error(`Incomplete trace: ${m[1]}`);
    const cells = rows[0].split("|");
    if (!cells[3].includes("UI-") || Number(cells[4].trim()) !== acCount)
      throw Error(`Trace disagrees with source: ${m[1]}`);
    const direct = [...journeyRefs]
      .filter(([, refs]) => refs.has(m[1]))
      .map(([id]) => id)
      .sort();
    const indexed = [...new Set(cells[5].match(/JRN-\d+/g) || [])].sort();
    if (direct.join(",") !== indexed.join(","))
      throw Error(`Explicit journey trace disagrees with source: ${m[1]}`);
    tracedFeatures++;
  }
  for (const m of text.matchAll(
    /^## (UI-[A-Z0-9-]+) [^\n]+\n([\s\S]*?)(?=^<a id="ui-|^## |(?![\s\S]))/gm,
  )) {
    const body = m[2];
    // Modules use different domain-specific labels for the same specification
    // dimensions. Require an explicit section/label, not a passing mention.
    const dimensions = {
      入口: ["目标到达路径", "目标入口", "导航入口", "入口", "目标入口与返回"],
      输入: [
        "字段与操作交付约定",
        "可编辑字段",
        "字段与操作规格",
        "字段与输入规则",
        "用户输入",
      ],
      动作: [
        "操作顺序",
        "按钮、前提与结果",
        "动作与结果",
        "动作与反馈",
        "按钮与动作",
      ],
      状态: [
        "关键状态",
        "业务状态与异常",
        "状态与异常",
        "状态与反馈",
        "状态与导航",
        "页面状态与异常",
        "适用状态与离开",
      ],
      返回: [
        "离开及返回",
        "离开、返回与跨模块上下文",
        "区域联动与返回",
        "跨模块联动与返回",
        "页面入口、布局与返回",
        "状态与导航",
        "适用状态与离开",
        "目标入口与返回",
      ],
      验收: ["业务验收要求", "验收标准", "可观察验收条件"],
    };
    for (const [dimension, labels] of Object.entries(dimensions)) {
      const namedSection = new RegExp(
        `^(?:#{3,4} +|\\*\\*)(?:${labels.join("|")})(?:[：:]\\*\\*|\\*\\*|$)`,
        "m",
      );
      if (!namedSection.test(body))
        throw Error(
          `Missing UI ${dimension} section (${labels.join(" / ")}): ${m[1]}`,
        );
    }
    // Inputs must have a real typed/default/required table; read-only facts may
    // have a separate table or be explicitly marked within the field table.
    if (!/^\|[^\n]*(?:类型|默认|初始|必填)[^\n]*\|/m.test(body))
      throw Error(`Missing UI typed input specification: ${m[1]}`);
    if (!/^(?:#{3,4} +[^\n]*只读[^\n]*|\|[^\n]*只读[^\n]*\|)/m.test(body))
      throw Error(`Missing UI explicit read-only facts: ${m[1]}`);
    if (!body.includes(`${m[1]}-AC01`))
      throw Error(`Missing UI acceptance: ${m[1]}`);
    const design = `designs/${m[1].toLowerCase()}.svg`;
    const relative = path
      .relative(path.dirname(file), path.join(root, design))
      .split(path.sep)
      .join("/");
    if (!body.includes(`](${relative})`))
      throw Error(`Missing own target SVG source: ${m[1]}`);
    const svg = await fs.readFile(path.join(root, design), "utf8");
    const marker = `data-ui-id="${m[1]}" data-design-source="${design}"`;
    if (prototype.split(marker).length !== 2)
      throw Error(`Target UI browser missing or duplicates ${m[1]}`);
    if (!prototype.includes(svg))
      throw Error(`Target UI browser does not embed source SVG: ${m[1]}`);
    specifiedPages++;
  }
  for (const m of text.matchAll(
    /^## (NFR-[A-Z0-9-]+) [^\n]+\n([\s\S]*?)(?=^## |(?![\s\S]))/gm,
  ))
    for (const field of [
      "环境与负载",
      "通过标准",
      "测量与证据",
      "超限与失败",
      "验收执行要求",
    ])
      if (!m[2].includes(`**${field}：**`))
        throw Error(`Missing NFR ${field}: ${m[1]}`);
}
if (tracedFeatures !== counts.FR || specifiedPages !== counts.UI)
  throw Error("Incomplete feature or UI delivery specifications");
if ((prototype.match(/data-ui-id="/g) || []).length !== counts.UI)
  throw Error("UI browser page count disagrees with source definitions");
console.log({
  documents: files.length,
  ...counts,
  acceptance: ac.length,
  decisions: decisionSources.size,
  tracedFeatures,
  specifiedPages,
  links: "valid",
  readerDefinitions: "complete",
  targetDesignPages: specifiedPages,
  explicitJourneyTrace: "valid",
});
