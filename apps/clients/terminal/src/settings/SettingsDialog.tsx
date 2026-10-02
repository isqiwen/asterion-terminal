import { useEffect, useId, useRef, type ReactNode } from "react";
// A settings form opens over the list: adding or editing never moves the
// items being looked at, and Escape or Cancel leaves them untouched.
export function SettingsDialog({
  title,
  busy,
  onCancel,
  onSubmit,
  children,
}: {
  title: string;
  busy: boolean;
  onCancel: () => void;
  onSubmit: (event: React.FormEvent<HTMLFormElement>) => void;
  children: ReactNode;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const heading = useId();
  useEffect(() => {
    const value = dialog.current!;
    value.showModal();
    return () => value.close();
  }, []);
  return (
    <dialog
      ref={dialog}
      className="service-action-dialog settings-dialog"
      aria-labelledby={heading}
      onCancel={event => {
        event.preventDefault();
        if (!busy) onCancel();
      }}
    >
      <h2 id={heading}>{title}</h2>
      <form onSubmit={onSubmit}>{children}</form>
    </dialog>
  );
}
