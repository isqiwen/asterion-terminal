import { useEffect, useRef, useState } from "react";
import { basicSetup, EditorView } from "codemirror";
import { EditorSelection, EditorState } from "@codemirror/state";
import { python } from "@codemirror/lang-python";
import { lab } from "../../src/bridge/lab";
import { ActionError, useActionError } from "../../src/host/action-error";
import { translate } from "../../src/i18n";

const t = (key: string) => translate("asterion.terminal.research", key);

const darkPalette: Record<string, string> = {
  "#404740": "#6e7989",
  "#708": "#c792ea",
  "#219": "#f78c6c",
  "#164": "#f78c6c",
  "#a11": "#c3e88d",
  "#e40": "#89ddff",
  "#00f": "#82aaff",
  "#30a": "#ffcb6b",
  "#085": "#ffcb6b",
  "#167": "#38d6f2",
  "#256": "#82aaff",
  "#00c": "#82aaff",
  "#940": "#94a3b8",
  "#f00": "#ff5c7a",
};

const darkSyntaxRules = (() => {
  const rules: Record<string, Record<string, string>> = {};
  const modules = EditorState.create({ extensions: [basicSetup] }).facet(EditorView.styleModule);
  for (const mod of modules) {
    for (const match of mod
      .getRules()
      .matchAll(/(\.ͼ[a-z0-9]+)\s*\{color:\s*(#[0-9a-fA-F]+);?\}/g)) {
      const mapped = darkPalette[match[2].toLowerCase()];
      if (mapped) rules[`& ${match[1]}`] = { color: mapped };
    }
  }
  return rules;
})();

const theme = EditorView.theme(
  {
    "&": { height: "100%", backgroundColor: "var(--canvas)", color: "var(--text)" },
    ".cm-content": { fontFamily: "var(--font-terminal)", caretColor: "var(--accent)" },
    ".cm-cursor, .cm-dropCursor": { borderLeftColor: "var(--accent)" },
    ".cm-activeLine": { backgroundColor: "rgb(255 255 255 / 3%)" },
    ".cm-activeLineGutter": { backgroundColor: "var(--hover)", color: "var(--text-strong)" },
    ".cm-gutters": { backgroundColor: "var(--panel)", color: "var(--muted)", border: "none" },
    ...darkSyntaxRules,
  },
  { dark: true },
);

function indentWithFourSpaces(editor: EditorView, outdent: boolean): boolean {
  const { state } = editor;
  if (state.readOnly) return true;
  if (!outdent && state.selection.ranges.every(range => range.empty)) {
    editor.dispatch(state.replaceSelection("    "));
    return true;
  }
  const changes: { from: number; to: number; insert: string }[] = [];
  const seenLines = new Set<number>();
  for (const range of state.selection.ranges) {
    const startLine = state.doc.lineAt(range.from).number;
    const endLine =
      range.to > range.from && state.doc.lineAt(range.to).from === range.to
        ? state.doc.lineAt(range.to).number - 1
        : state.doc.lineAt(range.to).number;
    for (let number = startLine; number <= endLine; ++number) {
      if (seenLines.has(number)) continue;
      seenLines.add(number);
      const line = state.doc.line(number);
      if (outdent) {
        const leading = line.text.match(/^ {1,4}/)?.[0].length ?? 0;
        if (leading > 0) changes.push({ from: line.from, to: line.from + leading, insert: "" });
      } else {
        changes.push({ from: line.from, to: line.from, insert: "    " });
      }
    }
  }
  if (changes.length > 0) {
    editor.dispatch({ changes, selection: EditorSelection.create(state.selection.ranges) });
  }
  return true;
}

function localModuleImports(content: string, currentPath: string): string[] {
  const found = new Set<string>();
  for (const match of content.matchAll(
    /(?:from|import)\s+(strategies|factors)\.([A-Za-z_][A-Za-z0-9_]*)/g,
  )) {
    const target = `${match[1]}/${match[2]}.py`;
    if (target !== currentPath) found.add(target);
  }
  return [...found];
}

// One module of the strategy directory, opened on its unsaved `draft` if it
// has one. A built-in module opens read only, without save or delete.
export function SourceEditor({
  repository,
  path,
  draft,
  edited,
  changed,
  deleted,
  opened,
}: {
  repository: string;
  path: string;
  draft: string | undefined;
  edited: (content: string | null) => void;
  changed: () => void;
  deleted: () => void;
  opened?: (path: string) => void;
}) {
  const host = useRef<HTMLDivElement>(null);
  const view = useRef<EditorView | null>(null);
  const stateRef = useRef<{
    draft: string | undefined;
    edited: (content: string | null) => void;
    save: () => Promise<unknown>;
  }>({ draft, edited, save: () => Promise.resolve() });
  stateRef.current.edited = edited;
  const [readOnly, setReadOnly] = useState(true);
  const [loadedText, setLoadedText] = useState("");
  const [copying, setCopying] = useState(false);
  const [copyName, setCopyName] = useState("");
  const dirty = draft !== undefined;
  const { error, attempt, clear } = useActionError();
  const save = () =>
    attempt(async () => {
      if (!view.current) return;
      const content = view.current.state.doc.toString();
      await lab({ case: "saveSource", value: { source: { repository, path }, content } });
      setLoadedText(content);
      edited(null);
      changed();
    });
  stateRef.current.save = save;
  useEffect(() => {
    let active = true;
    setCopying(false);
    void attempt(async () => {
      const source = await lab({ case: "readSource", value: { repository, path } });
      if (!active || !host.current) return;
      setReadOnly(source.readOnly);
      setLoadedText(source.content);
      view.current = new EditorView({
        parent: host.current,
        state: EditorState.create({
          doc: stateRef.current.draft ?? source.content,
          extensions: [
            basicSetup,
            python(),
            theme,
            EditorState.readOnly.of(source.readOnly),
            EditorView.domEventHandlers({
              keydown(event, editor) {
                if (
                  (event.ctrlKey || event.metaKey) &&
                  !event.altKey &&
                  event.key.toLowerCase() === "s"
                ) {
                  event.preventDefault();
                  if (!source.readOnly) void stateRef.current.save();
                  return true;
                }
                if (event.key === "Tab" && !event.ctrlKey && !event.metaKey && !event.altKey) {
                  event.preventDefault();
                  return indentWithFourSpaces(editor, event.shiftKey);
                }
                return false;
              },
            }),
            EditorView.updateListener.of(
              update => update.docChanged && stateRef.current.edited(update.state.doc.toString()),
            ),
          ],
        }),
      });
    });
    return () => {
      active = false;
      view.current?.destroy();
      view.current = null;
    };
  }, [repository, path, attempt]);
  const remove = async () => {
    if (!window.confirm(t("删除这个模块？删除后不能恢复。"))) return;
    await attempt(async () => {
      await lab({ case: "deleteSource", value: { repository, path } });
      deleted();
    });
  };
  const folder = path.startsWith("factors/") ? "factors" : "strategies";
  const stem = path.replace(/^(strategies|factors)\//, "").replace(/\.py$/, "");
  const helperPaths = localModuleImports(draft ?? loadedText, path);
  return (
    <section className="lab-source" aria-label={path}>
      <header>
        <code>{path}</code>
        {opened &&
          helperPaths.map(helper => (
            <button key={helper} className="lab-link" onClick={() => opened(helper)}>
              {helper}
            </button>
          ))}
        {readOnly ? (
          !copying && (
            <button
              onClick={() => {
                setCopyName(`${stem}_copy`);
                setCopying(true);
              }}
            >
              {t("复制为新模块")}
            </button>
          )
        ) : (
          <>
            <button className="primary" disabled={!dirty} onClick={() => void save()}>
              {t("保存")}
            </button>
            <button onClick={() => void remove()}>{t("删除")}</button>
          </>
        )}
      </header>
      {copying && (
        <form
          className="lab-new-module"
          onSubmit={event => {
            event.preventDefault();
            const nextPath = `${folder}/${copyName}.py`;
            const content = view.current?.state.doc.toString() ?? loadedText;
            void attempt(async () => {
              await lab({
                case: "saveSource",
                value: { source: { repository, path: nextPath }, content, create: true },
              });
              setCopying(false);
              changed();
              opened?.(nextPath);
            });
          }}
        >
          <input
            aria-label={t("模块名")}
            pattern="[A-Za-z_][A-Za-z0-9_]*"
            required
            autoFocus
            value={copyName}
            onChange={event => setCopyName(event.target.value)}
          />
          <button className="primary" type="submit">
            {t("创建")}
          </button>
          <button
            type="button"
            onClick={() => {
              setCopying(false);
              clear();
            }}
          >
            {t("取消")}
          </button>
        </form>
      )}
      <ActionError error={error} namespace="asterion.terminal.research" />
      <div className="lab-source-editor" ref={host} />
    </section>
  );
}
