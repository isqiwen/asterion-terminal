import { render, screen } from "@testing-library/react";
import { expect, test } from "vitest";
import { registerTerminalPlugins } from "../../src/host/plugin-registry";
import { readySnapshot } from "../../src/testing/core";
import { ArchivedDataPanel } from "./ArchivedDataPanel";
import { plugin } from "./plugin";

registerTerminalPlugins([plugin]);

test("an archive with unsupported versions explains that its files are preserved", async () => {
  const snapshot = readySnapshot();
  render(
    <ArchivedDataPanel
      snapshot={snapshot}
      busy={false}
      error=""
      navigate={() => {}}
      openSettings={() => {}}
      trade={async () => {}}
      query={async () => ({
        ...snapshot,
        history_datasets: [],
        history_datasets_unsupported: 2,
      })}
    />,
  );
  expect((await screen.findByRole("status")).textContent).toBe(
    "仓库中有 2 个版本不支持读取，原文件仍保留。日线请查看全市场数据。",
  );
});
