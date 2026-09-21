/** Credential-free request port supplied by the trusted composition layer. */
export interface RequestClient {
  readonly key: string;
  request<T>(path: string, body?: unknown, timeout?: number): Promise<T>;
  text(path: string, body?: string): Promise<string>;
}
export type RequestGrant = Readonly<{
  path: string;
  descendants: boolean;
  methods: readonly ("GET" | "POST")[];
}>;
