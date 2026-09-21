import type { TerminalPlugins } from "./plugins";
export function ApplicationHost<C, S>({
  plugins,
  screen,
}: {
  plugins: TerminalPlugins<C, S>;
  screen: string;
}) {
  const Screen = plugins.screens.get(screen).component;
  return <Screen />;
}
