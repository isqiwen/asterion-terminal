import type { UiComposition } from "./modules";
export function ApplicationHost<C, S>({
  composition,
  screen,
}: {
  composition: UiComposition<C, S>;
  screen: string;
}) {
  const Screen = composition.screens.get(screen).component;
  return <Screen />;
}
