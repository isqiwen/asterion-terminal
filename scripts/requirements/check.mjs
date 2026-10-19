import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../docs/requirements");
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
  texts = new Map(await Promise.all(files.map(async f => [f, await fs.readFile(f, "utf8")])));
const definitions = new Map(),
  anchors = new Map();
for (const [file, text] of texts) {
  const ids = [...text.matchAll(/<a id="([^"]+)"/g)].map(m => m[1]);
  if (ids.length !== new Set(ids).size) throw Error(`Duplicate anchor: ${file}`);
  anchors.set(file, new Set(ids));
  for (const match of text.matchAll(/^#{2,3} ((?:REQ|FR|UI|JRN|NFR)-[A-Z0-9-]+) (.+)$/gm)) {
    if (definitions.has(match[1])) throw Error(`Duplicate definition ${match[1]}`);
    definitions.set(match[1], { file, title: match[2] });
    if (!ids.includes(match[1].toLowerCase())) throw Error(`Missing anchor ${match[1]}`);
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
      if (!body.includes(`**${field}：**`)) throw Error(`Missing ${field}: ${match[1]}`);
    if (!body.includes(`${match[1]}-AC01`)) throw Error(`Missing acceptance: ${match[1]}`);
  }
}
const all = [...texts.values()].join("\n");
const ac = [...all.matchAll(/(?:\*\*|\| )((?:REQ|FR|UI)-[A-Z0-9-]+-AC\d+)(?:\*\*|\s*\|)/g)].map(
  m => m[1],
);
if (ac.length !== new Set(ac).size) throw Error("Duplicate acceptance definition");
const counts = Object.fromEntries(
  ["REQ", "FR", "UI", "JRN", "NFR"].map(prefix => [
    prefix,
    [...definitions.keys()].filter(id => id.startsWith(prefix + "-")).length,
  ]),
);
const html = await fs.readFile(path.join(root, "requirements.html"), "utf8");
for (const id of definitions.keys())
  if (!html.includes(`id="${id.toLowerCase()}"`)) throw Error(`Reader missing ${id}`);
const trace = texts.get(path.join(root, "traceability.md"));
const decisionIndex = texts.get(path.join(root, "decisions.md"));
const decisionIds = new Set();
let tracedFeatures = 0,
  specifiedPages = 0;
for (const [file, text] of texts) {
  if (["traceability.md", "decisions.md"].includes(path.basename(file))) continue;
  for (const m of text.matchAll(
    /^### (FR-[A-Z0-9-]+) [^\n]+\n([\s\S]*?)(?=^<a id=|(?![\s\S]))/gm,
  )) {
    const ui = m[2].match(/\*\*界面对应：\*\*([^\n]+)/)?.[1];
    if (!ui?.includes("UI-")) throw Error(`No UI mapping: ${m[1]}`);
    const acCount = new Set(m[2].match(new RegExp(m[1] + "-AC\\d+", "g")) || []).size;
    const rows = trace.split("\n").filter(row => row.startsWith(`| [${m[1]} `));
    if (!acCount || rows.length !== 1) throw Error(`Incomplete trace: ${m[1]}`);
    const cells = rows[0].split("|");
    if (!cells[3].includes("UI-") || Number(cells[4].trim()) !== acCount)
      throw Error(`Trace disagrees with source: ${m[1]}`);
    tracedFeatures++;
  }
  for (const m of text.matchAll(/^## (UI-[A-Z0-9-]+) [^\n]+\n([\s\S]*?)(?=^## |(?![\s\S]))/gm)) {
    for (const field of ["目标到达路径", "字段与操作交付约定", "关键状态", "交互验收案例"])
      if (!m[2].includes(field)) throw Error(`Missing UI ${field}: ${m[1]}`);
    specifiedPages++;
  }
  for (const m of text.matchAll(/^## (NFR-[A-Z0-9-]+) [^\n]+\n([\s\S]*?)(?=^## |(?![\s\S]))/gm))
    for (const field of ["环境与负载", "通过标准", "测量与证据", "超限与失败", "验证状态"])
      if (!m[2].includes(`**${field}：**`)) throw Error(`Missing NFR ${field}: ${m[1]}`);
  for (const m of text.matchAll(
    /^- \*\*(DEC-[A-Z0-9-]+)\*\*：\*\*(规格决定|运行配置|交付准入)\*\*。/gm,
  )) {
    if (decisionIds.has(m[1])) throw Error(`Duplicate decision source ${m[1]}`);
    decisionIds.add(m[1]);
    if (!decisionIndex.includes(`| ${m[1]} | ${m[2]} |`))
      throw Error(`Missing decision index ${m[1]}`);
  }
}
if (tracedFeatures !== counts.FR || specifiedPages !== counts.UI)
  throw Error("Incomplete feature or UI delivery specifications");
console.log({
  documents: files.length,
  ...counts,
  acceptance: ac.length,
  decisions: decisionIds.size,
  tracedFeatures,
  specifiedPages,
  links: "valid",
  readerDefinitions: "complete",
});
