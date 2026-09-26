import type { RequestGrant } from "@asterion/runtime-client/requests";

export const taskRequests: readonly RequestGrant[] = [
  { path: "/contract-roles/computed/tasks/:id/retry", descendants: false, methods: ["POST"] },
  { path: "/data/jobs", descendants: true, methods: ["GET"] },
  { path: "/data/jobs/:id/retry", descendants: false, methods: ["POST"] },
];
