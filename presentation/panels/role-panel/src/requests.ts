import type { RequestGrant } from "@asterion/runtime-client/requests";

export const roleRequests: readonly RequestGrant[] = [
  { path: "/contract-roles/hierarchy", descendants: false, methods: ["GET"] },
  { path: "/contract-roles/candidates/preview", descendants: false, methods: ["POST"] },
  { path: "/contract-roles/computed/preview", descendants: false, methods: ["POST"] },
  { path: "/contract-roles/computed/start", descendants: false, methods: ["POST"] },
  { path: "/data/catalog", descendants: false, methods: ["GET"] },
  { path: "/trading-time", descendants: false, methods: ["GET"] },
  { path: "/contract-roles/computed/:id/sync-plan", descendants: false, methods: ["GET"] },
  { path: "/contract-roles/computed/:id/sync-workflows", descendants: false, methods: ["GET"] },
  { path: "/contract-roles/computed/sync-batches", descendants: false, methods: ["POST"] },
  { path: "/contract-roles/computed/sync-workflows/:id", descendants: false, methods: ["GET"] },
  { path: "/contract-roles/computed/tasks/:id", descendants: false, methods: ["GET"] },
  { path: "/data/providers", descendants: false, methods: ["GET"] },
  { path: "/contract-roles", descendants: false, methods: ["GET"] },
  { path: "/contract-roles/computed", descendants: false, methods: ["GET"] },
];
