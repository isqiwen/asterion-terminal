import type { RequestGrant } from "@asterion/runtime-client/requests";

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
