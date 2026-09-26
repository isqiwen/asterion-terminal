import type { ReactNode } from "react";
export function Empty({ children, action }: { children: ReactNode; action?: ReactNode }) {
  return <div className="dashboard-empty"><p>{children}</p>{action}</div>;
}
