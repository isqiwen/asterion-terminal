import { useEffect, useRef, useState } from "react";
import { basicSetup, EditorView } from "codemirror";
import { EditorState } from "@codemirror/state";
import { python } from "@codemirror/lang-python";
import { ActionError, lab, translate, useActionError } from "../contract";

const t = (key: string) => translate("asterion.terminal.research", key);

const theme = EditorView.theme(
  {
    "&": { height: "100%", backgroundColor: "var(--canvas)", color: "var(--text)" },
    ".cm-content": { fontFamily: "var(--font-terminal)" },
    ".cm-gutters": { backgroundColor: "var(--panel)", color: "var(--muted)", border: "none" },
  },
  { dark: true },
);

// One module of the strategy directory. A built-in module is shown read only.
export function SourceEditor({
  repository,
  path,
  changed,
  deleted,
}: {
  repository: string;
  path: string;
  changed: () => void;
  deleted: () => void;
}) {
  const host = useRef<HTMLDivElement>(null);
  const view = useRef<EditorView | null>(null);
  const [readOnly, setReadOnly] = useState(true);
  const [dirty, setDirty] = useState(false);
  const { error, attempt } = useActionError();
  useEffect(() => {
    let active = true;
    void attempt(async () => {
      const source = await lab({ case: "readSource", value: { repository, path } });
      if (!active || !host.current) return;
      setReadOnly(source.readOnly);
      setDirty(false);
      view.current = new EditorView({
        parent: host.current,
        state: EditorState.create({
          doc: source.content,
          extensions: [
            basicSetup,
            python(),
            theme,
            EditorState.readOnly.of(source.readOnly),
            EditorView.updateListener.of(update => update.docChanged && setDirty(true)),
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
  const save = () =>
    attempt(async () => {
      const content = view.current!.state.doc.toString();
      await lab({ case: "saveSource", value: { source: { repository, path }, content } });
      setDirty(false);
      changed();
    });
  const remove = async () => {
    if (!window.confirm(t("删除这个模块？删除后不能恢复。"))) return;
    await attempt(async () => {
      await lab({ case: "deleteSource", value: { repository, path } });
      deleted();
    });
  };
  return (
    <section className="lab-source" aria-label={path}>
      <header>
        <code>{path}</code>
        {readOnly ? (
          <span className="lab-chip">{t("内置，只读")}</span>
        ) : (
          <>
            <button className="primary" disabled={!dirty} onClick={() => void save()}>
              {t("保存")}
            </button>
            <button onClick={() => void remove()}>{t("删除")}</button>
          </>
        )}
      </header>
      <ActionError error={error} />
      <div className="lab-source-editor" ref={host} />
    </section>
  );
}
