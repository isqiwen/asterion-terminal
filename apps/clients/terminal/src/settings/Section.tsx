import type { ReactNode } from "react";
// Every settings page is made of these: a titled block with an optional
// explanation, its actions at the top right, and its content below.
export function SettingsSection({
  title,
  badge,
  description,
  actions,
  label,
  className,
  children,
}: {
  title: string;
  badge?: ReactNode;
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
          <div className="settings-section-title">
            <h2>{title}</h2>
            {badge}
          </div>
          {description && <p>{description}</p>}
        </div>
        {actions && <div className="settings-actions">{actions}</div>}
      </header>
      {children}
    </section>
  );
}
