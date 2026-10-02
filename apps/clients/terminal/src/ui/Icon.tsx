import type { ReactNode } from "react";
const shapes = {
  star: <path d="m12 3 2.8 5.7 6.2.9-4.5 4.4 1.1 6.2L12 17.3l-5.6 2.9 1.1-6.2L3 9.6l6.2-.9Z" />,
  contract: (
    <>
      <path d="M6 3h8l4 4v14H6Z" />
      <path d="M14 3v5h4M9 12h6M9 16h6" />
    </>
  ),
  asterion: (
    <>
      <path d="m12 2 2.8 7.2L22 12l-7.2 2.8L12 22l-2.8-7.2L2 12l7.2-2.8Z" />
      <path d="M12 8v8M8 12h8" />
    </>
  ),
  market: (
    <>
      <path d="M3 18h18M4 13l5-5 4 4 7-8M15 4h5v5" />
    </>
  ),
  data: (
    <>
      <ellipse cx="12" cy="5" rx="8" ry="3" />
      <path d="M4 5v14c0 4 16 4 16 0V5M4 12c0 4 16 4 16 0" />
    </>
  ),
  research: (
    <>
      <path d="M9 3h6M10 3v6l-6 10a1.5 1.5 0 0 0 1.3 2h13.4a1.5 1.5 0 0 0 1.3-2L14 9V3M8 14h8" />
    </>
  ),
  trading: (
    <>
      <path d="M4 7h16m-4-4 4 4-4 4M20 17H4m4-4-4 4 4 4" />
    </>
  ),
  tasks: (
    <>
      <path d="m3 6 1.5 1.5L7 5M11 6h10M3 12h4M11 12h10M3 18h4M11 18h10" />
    </>
  ),
  settings: (
    <>
      <path d="m9 3-.7 2.3-2.4 1L3.8 6l-2 3.5L3.5 11v2L1.8 14.5l2 3.5 2.1-.3 2.4 1L9 21h6l.7-2.3 2.4-1 2.1.3 2-3.5-1.7-1.5v-2l1.7-1.5-2-3.5-2.1.3-2.4-1L15 3Z" />
      <circle cx="12" cy="12" r="3" />
    </>
  ),
  connections: (
    <>
      <rect x="3" y="3" width="18" height="6" rx="2" />
      <rect x="3" y="15" width="18" height="6" rx="2" />
      <path d="M12 9v6M7 6h.01M7 18h.01" />
    </>
  ),
  plugins: (
    <>
      <path d="M8 3v5M16 3v5M5 8h14v3a7 7 0 0 1-14 0ZM12 18v4" />
    </>
  ),
  info: (
    <>
      <circle cx="12" cy="12" r="9" />
      <path d="M12 11v6M12 7h.01" />
    </>
  ),
  close: <path d="m6 6 12 12M6 18 18 6" />,
  refresh: (
    <>
      <path d="M20 7v5h-5M4 17v-5h5M6 6a8 8 0 0 1 13 2M18 18a8 8 0 0 1-13-2" />
    </>
  ),
} satisfies Record<string, ReactNode>;
export type IconName = keyof typeof shapes;
/** Decorative icon. The surrounding control supplies its accessible name. */
export function Icon({ name, size = 18 }: { name: IconName; size?: number }) {
  return (
    <svg
      className="ui-icon"
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.5"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      focusable="false"
    >
      {shapes[name]}
    </svg>
  );
}
