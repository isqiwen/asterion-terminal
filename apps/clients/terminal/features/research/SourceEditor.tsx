import { useEffect, useRef, useState } from "react";
import { basicSetup } from "codemirror";
import {
  completeFromList,
  type Completion,
  type CompletionContext,
  type CompletionResult,
} from "@codemirror/autocomplete";
import { indentWithTab } from "@codemirror/commands";
import { python, pythonLanguage } from "@codemirror/lang-python";
import { HighlightStyle, indentUnit, syntaxHighlighting } from "@codemirror/language";
import { lintGutter, setDiagnostics, type Diagnostic } from "@codemirror/lint";
import { EditorState } from "@codemirror/state";
import {
  Decoration,
  EditorView,
  MatchDecorator,
  ViewPlugin,
  keymap,
  type ViewUpdate,
} from "@codemirror/view";
import { tags } from "@lezer/highlight";
import { lab } from "../../src/bridge/lab";
import { ActionError, useActionError } from "../../src/host/action-error";
import { type MessageValues, translate } from "../../src/i18n";

const t = (key: string, values?: MessageValues) =>
  translate("asterion.terminal.research", key, values);

const syntax = syntaxHighlighting(
  HighlightStyle.define([
    { tag: tags.meta, color: "#6e7989" },
    { tag: tags.keyword, color: "#c792ea" },
    {
      tag: [
        tags.atom,
        tags.bool,
        tags.url,
        tags.contentSeparator,
        tags.labelName,
        tags.literal,
        tags.inserted,
      ],
      color: "#f78c6c",
    },
    { tag: [tags.string, tags.deleted], color: "#c3e88d" },
    { tag: [tags.regexp, tags.escape, tags.special(tags.string)], color: "#89ddff" },
    {
      tag: [
        tags.definition(tags.variableName),
        tags.special(tags.variableName),
        tags.macroName,
        tags.definition(tags.propertyName),
      ],
      color: "#82aaff",
    },
    { tag: [tags.local(tags.variableName), tags.typeName, tags.namespace], color: "#ffcb6b" },
    { tag: tags.className, color: "#38d6f2" },
    { tag: tags.comment, color: "#94a3b8" },
    { tag: tags.invalid, color: "#ff5c7a" },
  ]),
);

const theme = EditorView.theme(
  {
    "&": { height: "100%", backgroundColor: "var(--field)", color: "var(--text)" },
    ".cm-content": {
      padding: "var(--space-2) 0",
      fontFamily: "var(--font-terminal)",
      caretColor: "var(--accent)",
    },
    ".cm-cursor, .cm-dropCursor": { borderLeftColor: "var(--accent)" },
    ".cm-activeLine": { backgroundColor: "var(--accent-surface)" },
    ".cm-activeLineGutter": { backgroundColor: "var(--hover)", color: "var(--text-strong)" },
    ".cm-gutters": {
      backgroundColor: "var(--panel)",
      color: "var(--muted)",
      borderRight: "1px solid var(--line)",
    },
    ".cm-tooltip": {
      backgroundColor: "var(--panel)",
      border: "1px solid var(--line-strong)",
      color: "var(--text)",
      fontFamily: "var(--font-terminal)",
    },
    ".cm-tooltip-autocomplete ul li[aria-selected]": {
      backgroundColor: "var(--accent-surface)",
      color: "var(--text-strong)",
    },
    ".cm-local-import": {
      textDecoration: "underline",
      textDecorationColor: "color-mix(in oklab, var(--accent) 42%, transparent)",
      textUnderlineOffset: "3px",
      cursor: "pointer",
    },
    ".cm-local-import:hover": {
      color: "var(--accent)",
      textDecorationColor: "var(--accent)",
    },
  },
  { dark: true },
);

const CONTEXT_COMPLETIONS: readonly Completion[] = [
  { label: "history", type: "method", detail: "(symbol, field, count) -> ndarray" },
  { label: "factor", type: "method", detail: "(fn, symbol, count, **settings) -> float" },
  { label: "target", type: "method", detail: "(symbol, lots)" },
  { label: "weights", type: "method", detail: "({symbol: weight})" },
  { label: "symbols", type: "property", detail: "tuple[str, ...]" },
  { label: "time", type: "property", detail: "datetime | None" },
];

const BAR_COMPLETIONS: readonly Completion[] = [
  { label: "symbol", type: "property", detail: "str" },
  { label: "contract", type: "property", detail: "str" },
  { label: "time", type: "property", detail: "datetime" },
  { label: "open", type: "property", detail: "float" },
  { label: "high", type: "property", detail: "float" },
  { label: "low", type: "property", detail: "float" },
  { label: "close", type: "property", detail: "float" },
  { label: "volume", type: "property", detail: "float" },
  { label: "open_interest", type: "property", detail: "float" },
  { label: "roll_yield", type: "property", detail: "float" },
];

const FIELD_COMPLETIONS: readonly Completion[] = [
  "open",
  "high",
  "low",
  "close",
  "volume",
  "open_interest",
  "roll_yield",
].map(label => ({ label, type: "constant", detail: "bar field" }));

const API_COMPLETIONS = completeFromList([
  { label: "Strategy", type: "class", detail: "asterion_strategy" },
  { label: "Bar", type: "class", detail: "asterion_strategy" },
  { label: "CombinedStrategy", type: "class", detail: "asterion_strategy" },
  { label: "asterion_strategy", type: "namespace" },
  { label: "ts_mean", type: "function", detail: "(values, window) -> ndarray" },
  { label: "ts_std", type: "function", detail: "(values, window, ddof=1) -> ndarray" },
  { label: "ts_delta", type: "function", detail: "(values, periods=1) -> ndarray" },
  { label: "ts_pct_change", type: "function", detail: "(values, periods=1) -> ndarray" },
  { label: "ts_rank", type: "function", detail: "(values, window) -> ndarray" },
  { label: "ts_zscore", type: "function", detail: "(values, window) -> ndarray" },
  { label: "ts_corr", type: "function", detail: "(x, y, window) -> ndarray" },
  { label: "decay_linear", type: "function", detail: "(values, window) -> ndarray" },
  { label: "winsorize_mad", type: "function", detail: "(scores, n=5.0) -> dict" },
  { label: "sector_neutralize", type: "function", detail: "(scores, sectors=None) -> dict" },
  { label: "orthogonalize", type: "function", detail: "(target, controls) -> dict" },
  { label: "product_sector", type: "function", detail: "(symbol) -> str" },
  { label: "cross_section_zscore", type: "function", detail: "(scores, clip=3.0) -> dict" },
  { label: "cross_section_rank", type: "function", detail: "(scores) -> dict" },
  { label: "liquid_symbols", type: "function", detail: "(ctx, bars, ...) -> list" },
  { label: "check_factor", type: "function", detail: "(fn, inputs, count, **settings)" },
  { label: "on_bar", type: "method", detail: "(self, ctx, bar)" },
  { label: "on_bars", type: "method", detail: "(self, ctx, bars)" },
  { label: "ready", type: "method", detail: "(self, ctx) -> bool" },
  { label: "params", type: "property", detail: "dict[str, bool | int | float | str]" },
  { label: "numpy", type: "namespace", detail: "2.3.5" },
  { label: "scipy", type: "namespace", detail: "1.17.1" },
]);

export function strategyCompletion(context: CompletionContext): CompletionResult | null {
  const member = context.matchBefore(/\b(ctx|bar)\.\w*$/);
  if (member) {
    const dot = member.text.indexOf(".");
    return {
      from: member.from + dot + 1,
      options: member.text.startsWith("ctx.") ? CONTEXT_COMPLETIONS : BAR_COMPLETIONS,
      validFor: /^\w*$/,
    };
  }
  const line = context.state.doc.lineAt(context.pos);
  const before = line.text.slice(0, context.pos - line.from);
  const quoted = before.match(/history\([^)]*["']([a-z_]*)$/);
  if (quoted) {
    return {
      from: context.pos - quoted[1]!.length,
      options: FIELD_COMPLETIONS,
      validFor: /^[a-z_]*$/,
    };
  }
  return API_COMPLETIONS(context) as CompletionResult | null;
}

export function tracebackDiagnostics(
  doc: EditorState["doc"],
  path: string,
  traceback: string | undefined,
): Diagnostic[] {
  if (!traceback || doc.lines < 1) return [];
  const lines = traceback.trim().split("\n");
  const message = lines.at(-1)?.trim();
  if (!message) return [];
  let lineNumber = 1;
  for (const match of traceback.matchAll(/File "([^"]+)", line (\d+)/g)) {
    if (match[1]!.replaceAll("\\", "/").endsWith(path)) {
      lineNumber = Number(match[2]);
    }
  }
  const line = doc.line(Math.min(Math.max(1, lineNumber), doc.lines));
  const from = line.from + (line.text.match(/^\s*/)?.[0].length ?? 0);
  return [{ from: Math.min(from, line.to), to: line.to, severity: "error", message }];
}

function importLinks(currentPath: string, readOnly: boolean) {
  const hint = navigator.platform.startsWith("Mac") ? "⌘+Click" : "Ctrl+Click";
  const matcher = new MatchDecorator({
    regexp: /(?:from|import)\s+((strategies|factors)\.([A-Za-z_][A-Za-z0-9_]*))/g,
    decorate(add, _from, to, match) {
      const target = `${match[2]}/${match[3]}.py`;
      if (target === currentPath) return;
      add(
        to - match[1]!.length,
        to,
        Decoration.mark({
          class: "cm-local-import",
          attributes: {
            "data-module": target,
            title: readOnly ? target : `${target} (${hint})`,
          },
        }),
      );
    },
  });
  return ViewPlugin.fromClass(
    class {
      decorations;
      constructor(view: EditorView) {
        this.decorations = matcher.createDeco(view);
      }
      update(update: ViewUpdate) {
        this.decorations = matcher.updateDeco(update, this.decorations);
      }
    },
    { decorations: instance => instance.decorations },
  );
}

// Puts the caret on a module's class or function, at the top of the editor.
function reveal(editor: EditorView, symbol: string | undefined) {
  if (!symbol) return;
  const at = editor.state.doc.toString().search(new RegExp(`^(?:class|def) ${symbol}\\b`, "m"));
  if (at < 0) return;
  editor.dispatch({
    selection: { anchor: at },
    effects: EditorView.scrollIntoView(at, { y: "start" }),
  });
}

// One module of the strategy directory, opened on its unsaved `draft` if it
// has one. A built-in module opens read only, without save.
export function SourceEditor({
  repository,
  path,
  draft,
  traceback,
  symbol,
  edited,
  saved,
  changed,
  opened,
  back,
  onBack,
  onDuplicate,
  onDelete,
}: {
  repository: string;
  path: string;
  draft: string | undefined;
  traceback?: string;
  symbol?: string;
  edited: (content: string | null) => void;
  saved: (content: string) => void;
  changed: () => void;
  opened?: (path: string) => void;
  back?: string;
  onBack?: () => void;
  onDuplicate?: (content: string) => void;
  onDelete?: () => void;
}) {
  const host = useRef<HTMLDivElement>(null);
  const view = useRef<EditorView | null>(null);
  const stateRef = useRef<{
    draft: string | undefined;
    symbol?: string;
    edited: (content: string | null) => void;
    opened?: (path: string) => void;
    save: () => Promise<unknown>;
  }>({ draft, symbol, edited, opened, save: () => Promise.resolve() });
  stateRef.current.draft = draft;
  stateRef.current.symbol = symbol;
  stateRef.current.edited = edited;
  stateRef.current.opened = opened;
  const loadedRef = useRef("");
  const savingRef = useRef(false);
  const syncingRef = useRef(false);
  const [loaded, setLoaded] = useState(false);
  const [readOnly, setReadOnly] = useState(true);
  const [loadedText, setLoadedText] = useState("");
  const [dirty, setDirty] = useState(draft !== undefined);
  const [armedDelete, setArmedDelete] = useState(false);
  const { error, attempt } = useActionError();
  const save = () =>
    attempt(async () => {
      const editor = view.current;
      if (!editor) return;
      const content = editor.state.doc.toString();
      savingRef.current = true;
      try {
        await lab({ case: "saveSource", value: { source: { repository, path }, content } });
      } finally {
        savingRef.current = false;
      }
      if (view.current === editor) {
        loadedRef.current = content;
        setLoadedText(content);
        setDirty(editor.state.doc.toString() !== content);
      }
      saved(content);
      changed();
    });
  stateRef.current.save = save;
  useEffect(() => {
    if (draft !== undefined || !view.current) return;
    const current = view.current.state.doc.toString();
    if (current !== loadedRef.current) {
      loadedRef.current = current;
      setLoadedText(current);
    }
    setDirty(false);
  }, [draft]);
  useEffect(() => {
    let active = true;
    void attempt(async () => {
      const source = await lab({ case: "readSource", value: { repository, path } });
      if (!active || !host.current) return;
      const initial = stateRef.current.draft ?? source.content;
      setReadOnly(source.readOnly);
      setLoaded(true);
      loadedRef.current = source.content;
      setLoadedText(source.content);
      setDirty(initial !== source.content);
      view.current = new EditorView({
        parent: host.current,
        state: EditorState.create({
          doc: initial,
          extensions: [
            basicSetup,
            ...(source.readOnly ? [] : [keymap.of([indentWithTab])]),
            indentUnit.of("    "),
            python(),
            pythonLanguage.data.of({ autocomplete: strategyCompletion }),
            importLinks(path, source.readOnly),
            lintGutter(),
            theme,
            syntax,
            EditorState.readOnly.of(source.readOnly),
            EditorView.contentAttributes.of({ "aria-label": path, tabindex: "0" }),
            EditorView.domEventHandlers({
              mousedown(event) {
                if (!source.readOnly && !(event.metaKey || event.ctrlKey)) return false;
                const element =
                  event.target instanceof Element ? event.target.closest(".cm-local-import") : null;
                const target = element?.getAttribute("data-module");
                if (!target || !stateRef.current.opened) return false;
                event.preventDefault();
                stateRef.current.opened(target);
                return true;
              },
              keydown(event) {
                if (
                  (event.ctrlKey || event.metaKey) &&
                  !event.altKey &&
                  event.key.toLowerCase() === "s"
                ) {
                  event.preventDefault();
                  if (!source.readOnly) void stateRef.current.save();
                  return true;
                }
                return false;
              },
            }),
            EditorView.updateListener.of(update => {
              if (!update.docChanged || syncingRef.current) return;
              const next = update.state.doc.toString();
              const nextDirty = next !== loadedRef.current;
              setDirty(nextDirty);
              stateRef.current.edited(next);
            }),
          ],
        }),
      });
      reveal(view.current, stateRef.current.symbol);
    });
    const reload = () => {
      if (!active || savingRef.current || stateRef.current.draft !== undefined || !view.current)
        return;
      void lab({ case: "readSource", value: { repository, path } })
        .then(source => {
          const editor = view.current;
          if (
            !active ||
            !editor ||
            savingRef.current ||
            stateRef.current.draft !== undefined ||
            source.content === loadedRef.current
          )
            return;
          loadedRef.current = source.content;
          setLoadedText(source.content);
          setReadOnly(source.readOnly);
          setDirty(false);
          syncingRef.current = true;
          editor.dispatch({
            changes: { from: 0, to: editor.state.doc.length, insert: source.content },
          });
          syncingRef.current = false;
        })
        .catch(() => {});
    };
    window.addEventListener("focus", reload);
    return () => {
      active = false;
      window.removeEventListener("focus", reload);
      view.current?.destroy();
      view.current = null;
    };
  }, [repository, path, attempt]);
  useEffect(() => {
    if (view.current) reveal(view.current, symbol);
  }, [symbol]);
  useEffect(() => {
    const editor = view.current;
    if (!editor) return;
    editor.dispatch(
      setDiagnostics(
        editor.state,
        dirty ? [] : tracebackDiagnostics(editor.state.doc, path, traceback),
      ),
    );
  }, [dirty, loadedText, path, traceback]);
  return (
    <section
      className="lab-source"
      aria-label={path}
      onMouseUp={event => {
        if (event.button === 3 && back && onBack) {
          event.preventDefault();
          onBack();
        }
      }}
      onKeyDownCapture={event => {
        if (!back || !onBack) return;
        const backKey = navigator.platform.startsWith("Mac")
          ? event.metaKey && !event.ctrlKey && !event.altKey && !event.shiftKey && event.key === "["
          : event.altKey &&
            !event.ctrlKey &&
            !event.metaKey &&
            !event.shiftKey &&
            event.key === "ArrowLeft";
        if (!backKey) return;
        event.preventDefault();
        event.stopPropagation();
        onBack();
      }}
    >
      <header>
        {back && onBack && (
          <button
            type="button"
            className="lab-link"
            aria-label={t("返回 {path}", { path: back })}
            title={t("返回 {path}", { path: back })}
            onClick={onBack}
          >
            ←
          </button>
        )}
        <code>{path}</code>
        {dirty && <span className="lab-unsaved">{t("未保存")}</span>}
        {readOnly ? (
          <>
            <span className="lab-readonly">{t("只读")}</span>
            {loaded && onDuplicate && (
              <button type="button" onClick={() => onDuplicate(loadedRef.current)}>
                {t("复制并编辑")}
              </button>
            )}
          </>
        ) : (
          <>
            {onDelete && (
              <button
                type="button"
                className={armedDelete ? "danger" : "danger-quiet"}
                aria-label={armedDelete ? undefined : t("删除 {name}", { name: path })}
                onBlur={() => setArmedDelete(false)}
                onClick={() => {
                  if (!armedDelete) {
                    setArmedDelete(true);
                    return;
                  }
                  setArmedDelete(false);
                  onDelete();
                }}
              >
                {armedDelete ? t("确认删除 {name}", { name: path }) : t("删除")}
              </button>
            )}
            <button type="button" className="primary" disabled={!dirty} onClick={() => void save()}>
              {t("保存")}
            </button>
          </>
        )}
      </header>
      <ActionError error={error} namespace="asterion.terminal.research" />
      <div className="lab-source-editor" ref={host} />
    </section>
  );
}
