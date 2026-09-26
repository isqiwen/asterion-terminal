import type { RequestClient } from "@asterion/runtime-client/requests";
import type { ReactNode } from "react";
import type { components } from "@asterion/api-types/schema";
export type Job = components["schemas"]["Job"];
export type TaskContext = {
  job: Job;
  api: RequestClient;
  onSubmitted: (job: Job) => void;
  onError: (error: string) => void;
};
export type TaskView = {
  id: string;
  title: string;
  result: (job: Job) => ReactNode;
  detail?: {
    title: string;
    render: (context: TaskContext, onBack: () => void) => ReactNode;
  };
  actions?: (context: TaskContext) => ReactNode;
};
