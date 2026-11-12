import { useRef, type InputHTMLAttributes } from "react";
import { translate } from "../i18n";
import { Icon } from "./Icon";

// A date written YYYY-MM-DD in every interface language. The browser's own
// date field formats by the system locale, which the language setting cannot
// change; its calendar still opens from the button.
export function DateInput({
  value,
  onChange,
  ...field
}: Omit<InputHTMLAttributes<HTMLInputElement>, "type" | "value" | "onChange"> & {
  value: string;
  onChange: (value: string) => void;
}) {
  const calendar = useRef<HTMLInputElement>(null);
  return (
    <span className="date-input">
      <input
        {...field}
        value={value}
        placeholder="YYYY-MM-DD"
        inputMode="numeric"
        onChange={event => onChange(event.target.value)}
      />
      <input
        ref={calendar}
        type="date"
        tabIndex={-1}
        aria-hidden
        value={/^\d{4}-\d{2}-\d{2}$/.test(value) ? value : ""}
        onChange={event => onChange(event.target.value)}
      />
      <button
        type="button"
        aria-label={translate("host", "选择日期")}
        disabled={field.disabled}
        onClick={() => calendar.current?.showPicker()}
      >
        <Icon name="calendar" size={14} />
      </button>
    </span>
  );
}
