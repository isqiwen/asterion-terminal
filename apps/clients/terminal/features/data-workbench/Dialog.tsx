import { useEffect, useRef, type ReactNode } from "react";

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
  useEffect(() => {
    const element = dialog.current;
    element?.showModal();
    return () => element?.close();
  }, []);
  return (
    <dialog
      ref={dialog}
      className="data-dialog"
      aria-label={title}
      onCancel={event => {
        event.preventDefault();
        close();
      }}
    >
      <h2>{title}</h2>
      {children}
    </dialog>
  );
}
