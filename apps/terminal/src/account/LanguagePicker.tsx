import { useEffect, useId, useRef, useState } from "react";

const languages = [
  { value: "zh", label: "简体中文" },
  { value: "en", label: "English" },
] as const;

export function LanguagePicker({
  value,
  onChange,
}: {
  value: string;
  onChange: (value: "zh" | "en") => void;
}) {
  const [open, setOpen] = useState(false);
  const root = useRef<HTMLDivElement>(null);
  const trigger = useRef<HTMLButtonElement>(null);
  const items = useRef<(HTMLButtonElement | null)[]>([]);
  const id = useId();
  const selected = languages.findIndex((item) => item.value === value);

  useEffect(() => {
    if (!open) return;
    items.current[selected]?.focus();
    const dismiss = (event: PointerEvent) => {
      if (!root.current?.contains(event.target as Node)) setOpen(false);
    };
    document.addEventListener("pointerdown", dismiss);
    return () => document.removeEventListener("pointerdown", dismiss);
  }, [open, selected]);

  return (
    <div
      className="account-language"
      ref={root}
      onBlur={(event) => {
        if (!event.currentTarget.contains(event.relatedTarget)) setOpen(false);
      }}
    >
      <button
        ref={trigger}
        className="language-trigger"
        type="button"
        aria-label={`Language / 语言: ${languages[selected]?.label}`}
        aria-haspopup="menu"
        aria-expanded={open}
        aria-controls={open ? id : undefined}
        onClick={() => setOpen(!open)}
        onKeyDown={(event) => {
          if (event.key === "ArrowDown" || event.key === "ArrowUp") {
            event.preventDefault();
            setOpen(true);
          }
        }}
      >
        {languages[selected]?.label}
        <svg viewBox="0 0 10 10" aria-hidden="true">
          <path d="m2 3.5 3 3 3-3" />
        </svg>
      </button>
      {open && (
        <div
          id={id}
          className="language-menu"
          role="menu"
          aria-label="Language / 语言"
          onKeyDown={(event) => {
            const current = items.current.indexOf(
              document.activeElement as HTMLButtonElement,
            );
            let next: number | undefined;
            if (event.key === "ArrowDown")
              next = (current + 1) % languages.length;
            if (event.key === "ArrowUp")
              next = (current + languages.length - 1) % languages.length;
            if (event.key === "Home") next = 0;
            if (event.key === "End") next = languages.length - 1;
            if (next !== undefined) {
              event.preventDefault();
              items.current[next]?.focus();
            }
            if (event.key === "Escape") {
              event.preventDefault();
              event.stopPropagation();
              setOpen(false);
              trigger.current?.focus();
            }
          }}
        >
          {languages.map((item, index) => (
            <button
              key={item.value}
              type="button"
              ref={(element) => {
                items.current[index] = element;
              }}
              role="menuitemradio"
              aria-checked={value === item.value}
              tabIndex={-1}
              onClick={() => {
                onChange(item.value);
                setOpen(false);
                trigger.current?.focus();
              }}
            >
              <span>{item.label}</span>
              <span aria-hidden="true">{value === item.value ? "✓" : ""}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
