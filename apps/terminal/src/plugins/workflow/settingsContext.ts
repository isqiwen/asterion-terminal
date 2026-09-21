import type { RequestClient } from "../../api/requests";
import type { Preferences } from "../appearance/preferences";
export type SettingsContext = {
  empty: Readonly<Record<string, never>>;
  appearance: Readonly<{ preferences: Preferences }>;
  extensions: Readonly<{ api: RequestClient }>;
  connections: Readonly<{ api: RequestClient }>;
  data: Readonly<{ api: RequestClient }>;
  services: Readonly<{
    connection: Readonly<{
      starting: boolean;
      connected: boolean;
      directory: string;
      setToken: (token: string) => void;
    }>;
    busy: boolean;
    toggleService: () => Promise<void>;
  }>;
};
