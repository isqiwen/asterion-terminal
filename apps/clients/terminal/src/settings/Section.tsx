import type { ReactNode } from "react";
// Every settings page is made of these: a titled block with an optional
// explanation, its actions at the top right, and its content below.
export function SettingsSection({
  title,
  description,
  actions,
  label,
  className,
  children,
}: {
  title: string;
  description?: ReactNode;
  actions?: ReactNode;
  label?: string;
  className?: string;
  children?: ReactNode;
}) {
  return (
    <section
      className={className ? `settings-section ${className}` : "settings-section"}
      aria-label={label ?? title}
    >
      <header className="settings-section-header">
        <div>
          <h2>{title}</h2>
          {description && <p>{description}</p>}
        </div>
        {actions && <div className="settings-actions">{actions}</div>}
      </header>
      {children}
    </section>
  );
}
