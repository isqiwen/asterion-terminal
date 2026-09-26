import type { RequestGrant } from "@asterion/runtime-client/requests";

export const extensionRequests: readonly RequestGrant[] = [
  { path: "/extensions", descendants: true, methods: ["GET", "POST"] },
];
