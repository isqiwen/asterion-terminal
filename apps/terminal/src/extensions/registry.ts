/** Trusted, statically composed contributions. Never loads third-party code. */
export class ContributionRegistry<T extends { id: string }> {
  private readonly entries = new Map<string, T>();
  constructor(contributions: readonly T[]) {
    for (const contribution of contributions) {
      if (!/^[a-z][a-z0-9_]*(?:[.-][a-z0-9_]+)+$/.test(contribution.id))
        throw new Error(`Invalid contribution ID: ${contribution.id}`);
      if (this.entries.has(contribution.id))
        throw new Error(`Duplicate contribution: ${contribution.id}`);
      this.entries.set(contribution.id, Object.freeze({ ...contribution }));
    }
  }
  get(id: string): T {
    const value = this.entries.get(id);
    if (!value) throw new Error(`Unknown contribution: ${id}`);
    return value;
  }
  all(): readonly T[] {
    return [...this.entries.values()];
  }
}

export type Command<C> = {
  id: string;
  title: string;
  key?: string;
  enabled: (context: C) => boolean;
  execute: (context: C) => void | Promise<void>;
};
export class CommandRegistry<C> extends ContributionRegistry<Command<C>> {
  constructor(commands: readonly Command<C>[]) {
    super(commands);
    const keys = commands.flatMap((command) =>
      command.key ? [command.key.toLowerCase()] : [],
    );
    if (new Set(keys).size !== keys.length)
      throw new Error("Duplicate command shortcut");
  }
  async execute(id: string, context: C): Promise<boolean> {
    const command = this.get(id);
    if (!command.enabled(context)) return false;
    await command.execute(context);
    return true;
  }
  shortcut(key: string): Command<C> | undefined {
    return this.all().find(
      (command) => command.key?.toLowerCase() === key.toLowerCase(),
    );
  }
}
