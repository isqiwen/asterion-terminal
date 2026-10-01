import { useEffect, useRef, type ReactNode } from "react";
import { t } from "./service-state";
export function ServiceActionDialog({
  title,
  children,
  busy,
  disabled,
  onCancel,
  onConfirm,
}: {
  title: string;
  children: ReactNode;
  busy: boolean;
  disabled?: boolean;
  onCancel: () => void;
  onConfirm: () => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    const value = dialog.current!;
    value.showModal();
    return () => value.close();
  }, []);
  return (
    <dialog
      ref={dialog}
      className="service-action-dialog"
      aria-labelledby="service-action-title"
      onCancel={event => {
        event.preventDefault();
        if (!busy) onCancel();
      }}
    >
      <h2 id="service-action-title">{title}</h2>
      {children}
      <div className="source-actions">
        <button autoFocus disabled={busy} onClick={onCancel}>
          {t("取消")}
        </button>
        <button className="primary" disabled={busy || disabled} onClick={onConfirm}>
          {busy ? t("正在处理…") : t("确认操作")}
        </button>
      </div>
    </dialog>
  );
}
