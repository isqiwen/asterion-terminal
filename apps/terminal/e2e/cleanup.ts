import { rm } from "node:fs/promises";

// Agent-managed services outlive a test and keep their ledger lock open until
// the isolated Agent stops. POSIX removes open files; Windows refuses, which
// is expected there: try once and leave the rest to the temporary directory.
export async function removeFolder(path: string) {
  if (process.platform === "win32") {
    await rm(path, { recursive: true, force: true }).catch(() => undefined);
    return;
  }
  await rm(path, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
}
