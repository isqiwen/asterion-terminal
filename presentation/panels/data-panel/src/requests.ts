import type { RequestGrant } from "@asterion/runtime-client/requests";

export const dataRequests: readonly RequestGrant[] = [
  { path: "/trading-time", descendants: true, methods: ["GET", "POST"] },
  { path: "/data", descendants: true, methods: ["GET", "POST"] },
  { path: "/reference", descendants: true, methods: ["GET", "POST"] },
  { path: "/imports", descendants: true, methods: ["GET", "POST"] },
];

export const sourceRequests: readonly RequestGrant[] = [
  { path: "/data/connections", descendants: true, methods: ["GET", "POST"] },
  { path: "/data/providers", descendants: true, methods: ["GET", "POST"] },
];
