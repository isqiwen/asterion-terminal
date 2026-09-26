/** UI-owned scoped inputs; this plugin never imports the product or workspace. */
export type PanelContext = {

};
export type SettingsContext = {
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
  empty: Readonly<Record<string, never>>;
};
