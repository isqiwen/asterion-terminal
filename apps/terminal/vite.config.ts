import { defineConfig } from "vitest/config";
import react from "@vitejs/plugin-react";
export default defineConfig({
  plugins: [react()],
  server: { strictPort: true },
  test: {
    exclude: ["**/node_modules/**", "../../**/node_modules/**"],
    include: ["../../presentation/*/src/**/*.test.{ts,tsx}", "../../presentation/panels/*/src/**/*.test.{ts,tsx}", "../../products/terminal/src/**/*.test.{ts,tsx}"],
  },
});
