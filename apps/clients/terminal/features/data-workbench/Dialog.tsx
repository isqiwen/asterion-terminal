import { useId, useLayoutEffect, useRef, type ReactNode } from "react";

// A modal over the data page; Escape closes it.
export function Dialog({
  title,
  close,
  children,
}: {
  title: string;
  close: () => void;
  children: ReactNode;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const heading = useId();
  useLayoutEffect(() => {
    const element = dialog.current;
    element?.showModal();
    return () => element?.close();
  }, []);
  return (
    <dialog
      ref={dialog}
      className="data-dialog"
      aria-labelledby={heading}
      onCancel={event => {
        event.preventDefault();
        close();
      }}
      onClose={event => {
        if (!event.currentTarget.open) close();
      }}
    >
      <h2 id={heading}>{title}</h2>
      {children}
    </dialog>
  );
}
