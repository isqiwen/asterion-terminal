import { rm } from "node:fs/promises";

// Agent-managed services outlive a test and keep their ledger lock open until
// the isolated Agent stops; open files can still be removed.
export async function removeFolder(path: string) {
  await rm(path, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
}
