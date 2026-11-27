// Function-level comparison and snippet extraction for Python factor modules.
// Changes to unrelated public factors in the same module do not flag drift,
// while edits to the factor itself, its decorators, private helpers, imports,
// module constants, or any sibling factor it calls are always detected.

type Block =
  { kind: "def"; name: string; isPublic: boolean; text: string } | { kind: "other"; text: string };

const DEF_HEADER = /^def\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(/;
const DECORATOR = /^@\S/;

// Advance triple-quote and bracket nesting state across one Python source line.
function scanLine(
  line: string,
  state: { triple: "" | '"""' | "'''"; depth: number },
): { triple: "" | '"""' | "'''"; depth: number } {
  let { triple, depth } = state;
  let quote: "" | '"' | "'" = "";
  for (let i = 0; i < line.length; i++) {
    const ch = line[i]!;
    if (triple) {
      if (ch === "\\") {
        i++;
      } else if (line.startsWith(triple, i)) {
        triple = "";
        i += 2;
      }
      continue;
    }
    if (quote) {
      if (ch === "\\") {
        i++;
      } else if (ch === quote) {
        quote = "";
      }
      continue;
    }
    if (ch === "#") break;
    if (line.startsWith('"""', i)) {
      triple = '"""';
      i += 2;
    } else if (line.startsWith("'''", i)) {
      triple = "'''";
      i += 2;
    } else if (ch === '"' || ch === "'") {
      quote = ch;
    } else if (ch === "(" || ch === "[" || ch === "{") {
      depth++;
    } else if ((ch === ")" || ch === "]" || ch === "}") && depth > 0) {
      depth--;
    }
  }
  return { triple, depth };
}

function splitTopLevelBlocks(source: string): Block[] {
  const lines = source.replace(/\r\n?/g, "\n").split("\n");
  const blocks: Block[] = [];
  let state: { triple: "" | '"""' | "'''"; depth: number } = { triple: "", depth: 0 };
  let otherBuf: string[] = [];
  let decoratorBuf: string[] = [];
  let activeDef: { name: string; lines: string[] } | null = null;

  const flushOther = () => {
    const text = otherBuf.join("\n").trim();
    if (text) blocks.push({ kind: "other", text });
    otherBuf = [];
  };
  const flushDef = () => {
    if (!activeDef) return;
    const text = activeDef.lines.join("\n").trimEnd();
    blocks.push({
      kind: "def",
      name: activeDef.name,
      isPublic: !activeDef.name.startsWith("_"),
      text,
    });
    activeDef = null;
  };

  for (const line of lines) {
    const atTop = state.triple === "" && state.depth === 0;
    const blank = line.trim() === "";
    const indented = /^\s/.test(line);

    if (activeDef) {
      // A top-level non-blank, non-indented line ends the current def block.
      if (atTop && !blank && !indented) {
        flushDef();
      } else {
        activeDef.lines.push(line);
        state = scanLine(line, state);
        continue;
      }
    }

    if (atTop && DECORATOR.test(line)) {
      decoratorBuf.push(line);
      state = scanLine(line, state);
      continue;
    }

    const defMatch = atTop ? DEF_HEADER.exec(line) : null;
    if (defMatch) {
      flushOther();
      activeDef = { name: defMatch[1]!, lines: [...decoratorBuf, line] };
      decoratorBuf = [];
      state = scanLine(line, state);
      continue;
    }

    if (decoratorBuf.length > 0) {
      // Decorators before a class or non-def statement stay in `other`.
      if (atTop && !blank && !indented) {
        otherBuf.push(...decoratorBuf);
        decoratorBuf = [];
      } else {
        decoratorBuf.push(line);
        state = scanLine(line, state);
        continue;
      }
    }

    otherBuf.push(line);
    state = scanLine(line, state);
  }

  if (decoratorBuf.length > 0) otherBuf.push(...decoratorBuf);
  flushDef();
  flushOther();
  return blocks;
}

function identifierPattern(name: string): RegExp {
  return new RegExp(`\\b${name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}\\b`);
}

function effectiveFactorSource(source: string, funcName: string): string {
  const blocks = splitTopLevelBlocks(source);
  const publicDefs = new Map<string, string>();
  for (const block of blocks) {
    if (block.kind === "def" && block.isPublic) publicDefs.set(block.name, block.text);
  }
  const kept = new Set<string>([funcName]);
  const queue = [funcName];
  while (queue.length > 0) {
    const current = queue.pop()!;
    const body = publicDefs.get(current);
    if (!body) continue;
    for (const candidate of publicDefs.keys()) {
      if (kept.has(candidate)) continue;
      if (identifierPattern(candidate).test(body)) {
        kept.add(candidate);
        queue.push(candidate);
      }
    }
  }
  return blocks
    .filter(block => block.kind === "other" || !block.isPublic || kept.has(block.name))
    .map(block => block.text)
    .join("\n\n");
}

const functionNameOf = (factorId?: string) => {
  if (!factorId) return "";
  const dot = factorId.lastIndexOf(".");
  return dot >= 0 ? factorId.slice(dot + 1) : factorId;
};

export function factorSourceChanged(
  snapshotCode: string,
  currentCode: string,
  factorId?: string,
): boolean {
  const normSnap = snapshotCode.replace(/\r\n?/g, "\n");
  const normCurr = currentCode.replace(/\r\n?/g, "\n");
  if (normSnap === normCurr) return false;
  const funcName = functionNameOf(factorId);
  if (!funcName) return true;
  return effectiveFactorSource(normSnap, funcName) !== effectiveFactorSource(normCurr, funcName);
}

// Extract just the evaluated factor's definition when the module holds multiple public factors.
export function factorFunctionSnippet(source: string, factorId?: string): string | null {
  const funcName = functionNameOf(factorId);
  if (!funcName) return null;
  const blocks = splitTopLevelBlocks(source);
  const publicCount = blocks.filter(block => block.kind === "def" && block.isPublic).length;
  if (publicCount <= 1) return null;
  const target = blocks.find(block => block.kind === "def" && block.name === funcName);
  return target ? target.text : null;
}
