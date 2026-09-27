import { rm } from "node:fs/promises";

// Agent-managed services outlive a test and keep their ledger lock open until
// the isolated Agent stops. POSIX removes open files; Windows refuses, which
// is expected there and must not fail the test.
export async function removeFolder(path: string) {
  try {
    await rm(path, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
  } catch (error) {
    const code = (error as NodeJS.ErrnoException).code;
    if (process.platform === "win32" && (code === "EBUSY" || code === "EPERM")) return;
    throw error;
  }
}
