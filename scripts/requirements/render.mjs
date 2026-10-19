import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { marked } from "marked";
import { generateIndexes } from "./indexes.mjs";
const out = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../docs/requirements");
await generateIndexes(out);
const read = async file => fs.readFile(path.join(out, file), "utf8");
const files = [];
const enqueue = file => {
  if (!files.includes(file)) files.push(file);
};
enqueue("README.md");
enqueue("product.md");
enqueue("delivery.md");
const index = await read("README.md");
const moduleFiles = [
  ...index.matchAll(/\]\((modules\/[^)]+\/README\.md|common\/README\.md)\)/g),
].map(m => m[1]);
const modules = [];
for (const file of [...new Set(moduleFiles)]) {
  const md = await read(file);
  enqueue(file);
  const children = [];
  for (const match of md.matchAll(/\[([^\]]+)\]\(([^)#]+\.md)\)/g)) {
    const target = path.posix.normalize(path.posix.join(path.posix.dirname(file), match[2]));
    if (path.posix.dirname(target) !== path.posix.dirname(file) || target.endsWith("/README.md"))
      continue;
    if (children.some(c => c.doc === target)) continue;
    children.push({ title: match[1], doc: target });
    enqueue(target);
  }
  modules.push({
    title: md.split("\n")[0].slice(2),
    doc: file,
    slug: file.split("/").at(-2),
    children,
  });
}
for (const file of ["workflows/README.md", "common/screens.md", "decisions.md", "traceability.md"])
  enqueue(file);
const entries = [];
for (const file of files) {
  const md = await read(file);
  for (const match of md.matchAll(/^#{2,3} ((?:REQ|FR|UI|JRN|NFR)-[A-Z0-9-]+) (.+)$/gm))
    entries.push({ id: match[1], title: match[2], doc: file });
}
const data = {
  modules,
  themes: entries.filter(x => x.id.startsWith("REQ-")),
  features: entries.filter(x => x.id.startsWith("FR-")),
  pages: entries.filter(x => x.id.startsWith("UI-")),
  journeys: entries.filter(x => x.id.startsWith("JRN-")),
};
const themes = data.themes;
const e = v =>
  String(v)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
const imagePage = relative => relative.replace("screens/actual-", "").replace(".png", "");
const ids = new Set();
let body = "";
for (const file of files) {
  const md = await fs.readFile(path.join(out, file), "utf8"),
    chapter = file.replace(".md", "").replaceAll("/", "-").toLowerCase();
  let html = marked.parse(md);
  html = html.replace(/<h([1-6])>(.*?)<\/h\1>/g, (_, level, title) => {
    let base = title
        .replace(/<[^>]+>/g, "")
        .toLowerCase()
        .replace(/[.：，、？（）/]/g, "")
        .replace(/\s+/g, "-"),
      id = base,
      n = 1;
    while (ids.has(id)) id = base + "-" + n++;
    ids.add(id);
    return `<h${level} id="${e(id)}">${title}</h${level}>`;
  });
  html = html.replace(/href="([^"#]+\.md)(#[^"]*)?"/g, (m, f, hash) =>
    files.includes(path.posix.normalize(path.posix.join(path.posix.dirname(file), f)))
      ? `href="${
          hash ||
          "#" +
            path.posix
              .normalize(path.posix.join(path.posix.dirname(file), f))
              .replace(".md", "")
              .replaceAll("/", "-")
              .toLowerCase()
        }"`
      : m,
  );
  for (const match of [
    ...html.matchAll(
      /<img src="((?:\.\.\/)*(?:examples|designs)\/[^\"]+\.svg|(?:\.\.\/)*screens\/[^\"]+\.png)" alt="([^\"]*)">/g,
    ),
  ]) {
    const [full, relative, alt] = match,
      buffer = await fs.readFile(path.resolve(out, path.dirname(file), relative));
    html = html.replace(
      full,
      relative.endsWith(".svg")
        ? `<figure><figcaption>${alt}</figcaption>${buffer.toString()}</figure>`
        : `<figure><figcaption>${alt} · 实际运行截图 · <a href="${path.posix.relative(path.posix.dirname(file), "prototype.html")}#${imagePage("screens/" + path.basename(relative))}">查看界面</a></figcaption><img loading="lazy" src="data:image/png;base64,${buffer.toString("base64")}" alt="${alt}"></figure>`,
    );
  }
  html = html.replace(/href="([^"]+)"/g, (whole, href) => {
    if (href.startsWith("#") || /^https?:/.test(href)) return whole;
    return 'href="' + path.posix.normalize(path.posix.join(path.posix.dirname(file), href)) + '"';
  });
  body += `<section class="chapter" id="${chapter}">${html}</section>`;
}
const css = `:root{color-scheme:light;--ink:#1c304d;--line:#dce4ee}*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:#f4f6f9;color:var(--ink);font:15px/1.85 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif}a{color:#285cb8;text-decoration:none}a:hover{text-decoration:underline}aside{position:fixed;inset:0 auto 0 0;width:290px;background:#172b47;color:#dbe6f4;overflow:auto;padding:25px 21px}.brand{font-size:21px;line-height:1.5;color:white}.meta{font-size:12px;margin:13px 0 20px;color:#c1cfe0}aside a{display:block;font-size:12px;line-height:1.6;color:#dbe6f4;margin:7px 0}aside summary{font-size:13px;color:#a9c7ed;cursor:pointer;margin-top:18px}aside input{width:100%;padding:9px;color:white;background:#243c5e;border:1px solid #547299;font:inherit;font-size:13px}.submodule{padding-left:14px}aside summary a{display:inline;font-size:13px;color:#a9c7ed}.search-result{padding-left:8px;border-left:1px solid #547299}#matches{font-size:12px;min-height:25px}main{margin-left:290px;padding:36px 38px;max-width:1660px}.chapter{background:white;border:1px solid var(--line);border-radius:9px;padding:30px 32px;margin-bottom:28px;overflow:hidden}h1{font-size:29px;line-height:1.45;margin-top:0}h2{font-size:23px;margin-top:50px;border-top:2px solid #d7e3f4;padding-top:22px}h3{font-size:18px;color:#34557e;margin-top:30px}h4{font-size:16px;color:#34557e}a[id],h2,h3,h4{scroll-margin-top:24px}table{border-collapse:collapse;width:100%;font-size:13px;line-height:1.7;margin:18px 0 25px;overflow-wrap:anywhere}td,th{border-bottom:1px solid var(--line);padding:11px 10px;text-align:left;vertical-align:top}th{background:#edf2f9;color:#29486e}tr:nth-child(even) td{background:#fafbfd}figure{border:1px solid var(--line);border-radius:6px;overflow:hidden;margin:24px 0}figcaption{padding:11px 15px;background:#edf2f9;font-size:12px}figure svg,figure img{display:block;width:100%;height:auto}hr{border:0;border-top:1px solid var(--line);margin:35px 0}@media(max-width:1000px){aside{position:static;width:auto;max-height:370px}main{margin:0;padding:18px}.chapter{padding:20px 16px}table{display:block;overflow:auto}}@media print{aside{display:none}main{margin:0;padding:0}.chapter{border:0;padding:0;overflow:visible}h2{break-before:page}figure,tr{break-inside:avoid}figure{max-width:18cm}body{background:white;font-size:10pt}h1{font-size:22pt}table{font-size:8pt}}`;
const links = entries.map(v => ({
  id: v.id,
  title: v.title,
  search: v.id + " " + v.title + " " + v.title,
}));
const chapterLink = f => "#" + f.replace(".md", "").replaceAll("/", "-").toLowerCase();
const moduleNavigation = data.modules
  .map(
    m =>
      `${m.slug === "data" ? "<strong>核心业务模块</strong>" : m.slug === "system" ? "<strong>系统支撑模块</strong>" : m.slug === "common" ? "<strong>公共规范</strong>" : ""}<details open><summary><a href="${chapterLink(m.doc)}">${e(m.title)}</a></summary>${m.children.map(c => `<a class="submodule" href="${chapterLink(c.doc)}">${e(c.title)}</a>`).join("")}</details>`,
  )
  .join("");
const html = `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>个人期货量化研究与实盘交易 · 完整需求规格</title><style>${css}</style></head><body><aside><div class="brand">个人期货量化研究<br>与实盘交易</div><div class="meta">需求交付版 / 目标与实际分列 · 2026-10-18<br>${themes.length} 业务主题 / ${data.features.length} 功能 / ${data.pages.length} 界面对应 / ${data.journeys.length} 旅程</div><a href="prototype.html">打开实际界面浏览 ↗</a><label for="search">查找功能、页面、操作流程</label><input id="search" placeholder="编号 / 名称 / 关键词" type="search"><div id="matches"></div><div id="searchResults" hidden>${links.map(l => `<a class="search-result" data-search="${e(l.search)}" href="#${l.id.toLowerCase()}">${e(l.id + " " + l.title)}</a>`).join("")}</div><a href="#readme">总纲与目录</a><a href="#product">产品定位与范围</a><a href="#delivery">交付与验收</a><a href="#traceability">全量需求追踪</a>${moduleNavigation}<a href="#workflows-readme">跨模块业务流程</a><a href="#common-screens">实际界面与操作</a><a href="#decisions">决定与配置登记</a><details open><summary>界面需求对应</summary>${data.pages.map(p => `<a href="#${p.id.toLowerCase()}">${e(p.id + " " + p.title)}</a>`).join("")}</details><details><summary>完整操作旅程</summary>${data.journeys.map(j => `<a href="#${j.id.toLowerCase()}">${e(j.title)}</a>`).join("")}</details></aside><main>${body}</main><script>const input=document.querySelector('#search'),results=document.querySelector('#searchResults'),links=[...document.querySelectorAll('.search-result')];input.addEventListener('input',()=>{const q=input.value.trim().toLowerCase();results.hidden=!q;let n=0;for(const a of links){const hit=a.dataset.search.toLowerCase().includes(q);a.hidden=!hit;a.style.display=hit?'block':'none';if(hit)n++}document.querySelector('#matches').textContent=q?n+' 项匹配':''});</script></body></html>`;
await fs.writeFile(path.join(out, "requirements.html"), html);
console.log(
  "Reader written: " + Buffer.byteLength(html) + " bytes / " + files.length + " chapters",
);
