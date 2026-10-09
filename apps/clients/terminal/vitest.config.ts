import { defineConfig } from "vitest/config";
import terminal from "./vite.config";

// Unit and component tests: the Terminal's sources under jsdom. They take the
// application's module aliases but none of its plugins, so no C++ core starts;
// a test supplies the replies of the one transport it needs.
export default defineConfig({
  root: terminal.root,
  resolve: terminal.resolve,
  test: {
    include: ["src/**/*.test.{ts,tsx}", "plugins/**/*.test.{ts,tsx}"],
    environment: "jsdom",
    setupFiles: ["src/testing/setup.ts"],
  },
});
