import type { RequestGrant } from "../../api/requests";
// Product assembly grants. The transport mechanism contains no feature names.
export const dataRequests: readonly RequestGrant[] = [
  { path: "/trading-time", descendants: true, methods: ["GET", "POST"] },
  { path: "/data", descendants: true, methods: ["GET", "POST"] },
  { path: "/reference", descendants: true, methods: ["GET", "POST"] },
  { path: "/imports", descendants: true, methods: ["GET", "POST"] },
];
export const researchRequests: readonly RequestGrant[] = [
  { path: "/reference/releases", descendants: false, methods: ["GET"] },
  { path: "/reference/releases/:id", descendants: false, methods: ["GET"] },
  { path: "/trading-time", descendants: true, methods: ["GET", "POST"] },
  { path: "/contract-rules", descendants: true, methods: ["GET", "POST"] },
  { path: "/research", descendants: true, methods: ["GET", "POST"] },
  { path: "/data/catalog", descendants: true, methods: ["GET"] },
  { path: "/data/versions/:id", descendants: false, methods: ["GET"] },
  {
    path: "/data/versions/:id/coverage",
    descendants: false,
    methods: ["POST"],
  },
  { path: "/data/coverage/:id", descendants: false, methods: ["GET"] },
  {
    path: "/data/coverage/:id/refill-status",
    descendants: false,
    methods: ["GET"],
  },
  { path: "/data/coverage/:id/refill", descendants: false, methods: ["POST"] },
  { path: "/data/preparations", descendants: false, methods: ["GET"] },
  { path: "/data/jobs/:id/retry", descendants: false, methods: ["POST"] },
];
export const taskRequests: readonly RequestGrant[] = [
  { path: "/contract-roles/computed/tasks/:id/retry", descendants: false, methods: ["POST"] },
  { path: "/data/jobs", descendants: true, methods: ["GET"] },
  { path: "/data/jobs/:id/retry", descendants: false, methods: ["POST"] },
];
export const sourceRequests: readonly RequestGrant[] = [
  { path: "/data/connections", descendants: true, methods: ["GET", "POST"] },
  { path: "/data/providers", descendants: true, methods: ["GET", "POST"] },
];

export const extensionRequests: readonly RequestGrant[] = [
  { path: "/extensions", descendants: true, methods: ["GET", "POST"] },
];

export const roleRequests: readonly RequestGrant[] = [
  { path: "/contract-roles/computed/:id/sync-plan", descendants: false, methods: ["GET"] },
  { path: "/contract-roles/computed/:id/sync-workflows", descendants: false, methods: ["GET"] },
  { path: "/contract-roles/computed/sync-batches", descendants: false, methods: ["POST"] },
  { path: "/contract-roles/computed/sync-workflows/:id", descendants: false, methods: ["GET"] },
  { path: "/contract-roles/computed/tasks/:id", descendants: false, methods: ["GET"] },
  { path: "/data/providers", descendants: false, methods: ["GET"] },
  { path: "/contract-roles", descendants: false, methods: ["GET"] },
  { path: "/contract-roles/computed", descendants: false, methods: ["GET"] },
];
