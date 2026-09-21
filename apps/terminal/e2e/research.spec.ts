import { coverageIdentity } from "./reference-fixture";
import { strategies } from "./strategy-fixture";
import { rules, rulesRoute, selectRules, timeVersion } from "./rules-fixture";
import { test, expect } from "@playwright/test";
import { nativeContext } from "./native";

for (const local of [false, true]) {
  test(`fixed daily version, explicit assumptions, result, rerun and comparison (local=${local})`, async ({
    context,
    page,
  }) => {
    await nativeContext(context);
    const version = {
      id: "version-fixed-1",
      dataset_id: "dataset-1",
      rows: 50,
      manifest: {
        version_semantics: local ? "SNAPSHOT" : "CUMULATIVE",
        source: local ? "local_file" : "tushare",
        first: "2024-01-01",
        last: "2024-03-01",
        scope: { contract: "SHFE.rb2405" },
      },
    };
    const summary = {
      final_equity: "99946",
      net_profit: "-54",
      return_rate: "-0.00054",
      max_drawdown: "0.00054",
      fees: "4",
      fill_count: 2,
      open_lots: 0,
    };
    const runs: any[] = [];
    let listedStrategies: unknown[] = strategies;
    let experiment: any = null;
    let experimentInput: any;
    let validationRecord: any = null;
    let frozen: any;
    let coverage: any;
    const exported = '{"created_at":1.0,"bars":null}';
    const exportFlags: string[] = [];
    let packageStatus = "READY",
      replayCount = 0;

    await context.route("**/api/v1/**", (route) => {
      if (route.request().url().endsWith("/trading-time")) return route.fulfill({json:[timeVersion]});
      if (
        new URL(route.request().url()).pathname.endsWith("/research/strategies")
      )
        return route.fulfill({ json: listedStrategies });
      if (route.request().url().endsWith("/contract-rules"))
        return rulesRoute(route);
      if (route.request().url().endsWith("/access/scopes"))
        return route.fulfill({
          json: { token: "scope-fixture", expires: Date.now() / 1000 + 300 },
        });

      const req = route.request();
      const path = new URL(req.url()).pathname;
      if (path.endsWith("/reference/releases")) return route.fulfill({json:[{id:"rule-catalog", sources:["tushare"], contracts:1}]});
      if (path.endsWith("/reference/releases/rule-catalog")) return route.fulfill({json:{id:"rule-catalog",catalog:coverageIdentity().catalog}});
      if (path.endsWith("/contract-rules/settlement/preview")) {
        expect(req.postDataJSON()).toEqual({
          version_id: "settlement-fixed",
          contract_id: rules.spec.contract.id,
          trading_day: "2023-12-29",
        });
        return route.fulfill({
          json: {
            provider: "tushare",
            version_id: "settlement-fixed",
            checksum: "c".repeat(64),
            connection_id: null,
            observed_at: "2026-09-20T00:00:00Z",
            contract: rules.spec.contract,
            row: {
              symbol: "RB2405.SHF",
              contract: "SHFE.RB2405",
              exchange: "SHFE",
              trading_day: "2023-12-29",
              settle: "3800",
              trading_fee_rate: "0.050",
              trading_fee: "3",
              long_margin_rate: "12",
              short_margin_rate: "12",
              b_hedging_margin_rate: null,
              s_hedging_margin_rate: null,
              delivery_fee: null,
              offset_today_fee: null,
            },
          },
        });
      }
      if (path.endsWith("/contract-rules/settlement/confirm")) {
        const body = req.postDataJSON();
        expect(body.fee_unit).toBe("permille");
        expect(body.margin_unit).toBe("percent");
        expect(body.fee_scope).toBe("long_open_and_non_today_close");
        expect(body.availability_assumption).toBe("after_source_day");
        const { start, end, ...basis } = body;
        return route.fulfill({
          json: {
            start,
            end,
            fee_mode: "notional",
            open_fee: "0.00005",
            close_fee: "0.00005",
            margin_rate: "0.12",
            settlement_basis: basis,
          },
        });
      }
      if (
        path.includes("/data/catalog") &&
        new URL(req.url()).searchParams.get("type_id") === "futures.settlement"
      )
        return route.fulfill({
          json: {
            items: [
              {
                id: "settlement-fixed",
                manifest: {
                  scope: { symbol: "RB2405.SHF" },
                  first: "2023-12-29",
                  last: "2023-12-29",
                },
              },
            ],
            total: 1,
          },
        });
      if (path.endsWith("/contract-rules/source/preview")) {
        expect(req.postDataJSON()).toEqual({
          version_id: "contracts-fixed",
          contract_id: rules.spec.contract.id,
        });
        return route.fulfill({
          json: {
            contract: rules.spec.contract,
            suggested_multiplier: "10",
            multiplier_note: "报价单位匹配，使用每手数量",
            missing: ["最小变动价位", "生效期间", "手续费", "保证金比例"],
            basis: {
              contract_id: rules.spec.contract.id,
              provider: "tushare",
              version_id: "contracts-fixed",
              checksum: "b".repeat(64),
              connection_id: "c_" + "b".repeat(32),
              symbol: "RB2405.SHF",
              exchange: "SHFE",
              name: "螺纹钢测试",
              listed: "2023-01-01",
              delisted: "2024-05-15",
              trade_unit: "吨",
              per_unit: "10",
              multiplier: null,
              quote_unit: "元/吨",
              quote_unit_desc: "1元/吨",
            },
          },
        });
      }
      if (
        path.includes("/data/catalog") &&
        new URL(req.url()).searchParams.get("type_id") === "futures.contracts"
      )
        return route.fulfill({
          json: {
            items: [
              {
                id: "contracts-fixed",
                created_at: 1,
                manifest: {
                  scope: {
                    exchange: "SHFE",
                    connection_id: "c_" + "b".repeat(32),
                  },
                },
              },
            ],
            total: 1,
          },
        });
      if (path.includes("/security")) return route.fallback();
      if (path.endsWith("/research/experiments") && req.method() === "GET") return route.fulfill({ json: { items: experiment ? [experiment] : [] } });
      if (path.endsWith("/research/experiments") && req.method() === "POST") {
        experimentInput = req.postDataJSON();
        experiment = { spec: experimentInput, id: "experiment-1", name: experimentInput.name, runs: ["combination-1", "combination-2"], created_at: 1,
          items: experimentInput.grid.lookback.map((value: number, index: number) => ({ id: `combination-${index + 1}`, state: index === 0 ? "SUCCEEDED" : "RUNNING", parameters: { ...experimentInput.base.parameters, lookback: value }, error: null, result: index === 0 ? summary : null })) };
        return route.fulfill({ json: experiment });
      }
      if (path.endsWith("/research/experiments/experiment-1/validation") && req.method() === "GET") return route.fulfill({ json: validationRecord });
      if (path.endsWith("/research/experiments/experiment-1/validation") && req.method() === "POST") {
        const selection = req.postDataJSON();
        expect(selection).not.toHaveProperty("parameters");
        validationRecord = { run_id: "validation-1", created_at: 1, selection,
          evidence: { research: { request: experimentInput.base, summary }, warmup_start: "2024-04-01", warmup_end: "2024-04-04", evaluation_start: "2024-04-05", evaluation_end: "2024-04-10", warmup_bars: 4 },
          validation: { state: "SUCCEEDED", error: null, result: { ...summary, net_profit: "12" } } };
        return route.fulfill({ json: validationRecord });
      }
      if (path.endsWith("/research/experiments/experiment-1/cancel")) { experiment.items[1].state = "CANCELLED"; return route.fulfill({ json: experiment }); }
      if (path.endsWith("/research/experiments/experiment-1")) return route.fulfill({ json: experiment });
      if (path.endsWith("/research/workspace"))
        return route.fulfill({ json: { draft: null, templates: [] } });
      if (path.endsWith("/research/workspace/draft"))
        return route.fulfill({
          json: {
            id: "draft",
            ...req.postDataJSON(),
            revision: req.postDataJSON().expected_revision + 1,
          },
        });
      if (path.endsWith("/refill-status"))
        return route.fulfill({
          json: {
            report_id: path.split("/").at(-2),
            submitted: false,
            statuses: [],
            jobs: [],
            total: 0,
            truncated: false,
          },
        });
      if (path.endsWith("/coverage")) {
        const body = req.postDataJSON();
        expect(body.use_latest_daily).toBe(false);
        if (local) {
          expect(body.reference_policy).toBe("explicit_external");
          expect(body.calendar_version_id).toBe("fixed-calendar");
          expect(body.contracts_version_id).toBe("fixed-contracts");
          expect(body.reference_symbol).toBe("RB2405.SHF");
          expect(body.reference_note).toBe("文件与依据为同一交易所实际合约");
        }
        coverage = {
          id: `report-${body.end}`,
          daily_version_id: version.id,
          start: body.start,
          end: body.end,
          checker: "daily-coverage-v2",
          identity: coverageIdentity(), reference_symbol: local ? body.reference_symbol : null,
          created_at: 1,
          as_of: "2026-09-19",
          calendar_version_id: "fixed-calendar",
          contracts_version_id: "fixed-contracts",
          reference_policy: local ? "explicit_external" : "same_source",
          reference_note: local ? body.reference_note : "",
          refill_supported: !local,
          references: {},
          source: local ? "local_file" : "tushare",
          exchange: "SHFE",
          symbol: "rb2405",
          status: body.end === "2024-03-02" ? "GAPS" : "COVERED",
          counts:
            body.end === "2024-03-02"
              ? { PRESENT: 50, GAP: 1 }
              : { PRESENT: 50 },
          days:
            body.end === "2024-03-02"
              ? [
                  {
                    date: "2024-03-02",
                    status: "GAP",
                    has_data: false,
                    reason: "预期交易日缺失",
                  },
                ]
              : [],
          notes: ["仅相对于固定依据版本"],
          refill_ranges: [],
        };
        return route.fulfill({ json: coverage });
      }
      if (path.includes("/data/catalog"))
        return route.fulfill({
          json: { items: [version], total: 1, offset: 0 },
        });
      if (path.endsWith("/research/runs") && req.method() === "GET")
        return route.fulfill({ json: runs });
      if (path.endsWith("/research/runs") || path.endsWith("/rerun")) {
        const body = req.postDataJSON();
        if (!path.endsWith("/rerun")) {
          expect(body.version_id).toBe(version.id);
          expect(body.assumption).toBe("historical-close-unverified-calendar");
          expect(body.rules.spec.multiplier).toBe("10");
          expect(body.coverage_report_id).toBe(coverage.id);
          expect(body.coverage_policy).toBe("require_complete");
          frozen = body;
        }
        const run = {
          id: `run-${runs.length + 1}`,
          state: "SUCCEEDED",
          kind: "research.backtest",
          created_at: 1,
          attempt: 1,
          contract: "SHFE.rb2405",
          request: frozen,
          input_checksum: "frozen-input-checksum",
          engine: "daily-rules-next-open.v2",
          error: null,
          result: summary,
        };
        runs.unshift(run);
        return route.fulfill({ json: run, status: 202 });
      }
      if (path.endsWith("/export")) {
        exportFlags.push(new URL(req.url()).searchParams.get("include_data")!);
        return route.fulfill({
          contentType: "application/json",
          body: exported,
        });
      }
      if (path.endsWith("/results-archive"))
        return route.fulfill({
          json: { filename: "research-results.zip", content_base64: "UEs=" },
        });
      if (path.endsWith("/packages") || path.endsWith("/check")) {
        if (path.endsWith("/packages")) expect(req.postData()).toBe(exported);
        return route.fulfill({
          json: {
            id: "package-1",
            status: packageStatus,
            detail: "embedded",
            can_replay: packageStatus === "READY",
            engine: "daily-rules-next-open.v2",
            strategy: strategies[0].identity,
            version_id: version.id,
            origin_run_id: "run-1",
            output_checksum: "expected-checksum",
          },
        });
      }
      if (path.endsWith("/replay")) {
        replayCount++;
        return route.fulfill({ status: 202, json: runs[0] });
      }
      if (path.includes("/research/runs/")) {
        const run = runs.find((r) => path.endsWith(r.id));
        return route.fulfill({
          json: {
            ...run,
            version,
            coverage,
            warnings: ["历史收盘可用假设"],
            output: {
              summary,
              curve: [
                {
                  day: "2024-01-01",
                  equity: "100000",
                  settle: "20",
                  opening_balance: "100000",
                  settlement_pnl: "0",
                  fees: "0",
                  balance: "100000",
                  close_pnl: "0",
                  close_equity: "100000",
                  drawdown: "0",
                  position: 0,
                  margin: "0",
                  free_cash: "100000",
                },
                {
                  day: "2024-03-01",
                  equity: "99946",
                  settle: "16",
                  opening_balance: "99988",
                  settlement_pnl: "-40",
                  fees: "2",
                  balance: "99946",
                  close_pnl: "0",
                  close_equity: "99946",
                  drawdown: "0.00054",
                  position: 0,
                  margin: "0",
                  free_cash: "99946",
                },
              ],
              fills: [
                {
                  day: "2024-01-03",
                  side: "BUY",
                  lots: 1,
                  price: "21",
                  fee: "2",
                  reason: "上一根日线信号",
                },
              ],
              events: [],
            },
          },
        });
      }
      const user = {
        email: "research@example.com",
        first_name: "Research",
        last_name: "Test",
      };
      return route.fulfill({
        json: path.endsWith("/login")
          ? { session: "session", user }
          : path.endsWith("/me")
            ? user
            : path.endsWith("/health")
              ? { status: "ready" }
              : [],
      });
    });
    async function fillReferences() {
      await expect(
        page.getByRole("button", { name: "核对所选版本覆盖", exact: true }),
      ).toBeDisabled();
      await page
        .getByLabel("关联日历版本", { exact: true })
        .fill("fixed-calendar");
      await page
        .getByLabel("关联合约资料版本", { exact: true })
        .fill("fixed-contracts");
      await page.getByLabel("来源合约代码", { exact: true }).fill("RB2405.SHF");
      await page
        .getByLabel("关联说明", { exact: true })
        .fill("文件与依据为同一交易所实际合约");
    }
    await page.goto("/");
    await page.getByLabel("邮箱", { exact: true }).fill("research@example.com");
    await page
      .getByLabel("密码", { exact: true })
      .fill("test-research-password");
    await page.getByRole("button", { name: "登录", exact: true }).click();
    await page.getByRole("button", { name: "研究", exact: true }).click();
    const installedStrategy = { ...strategies[1], name: "外部动量", identity: {
      id: "example.close_momentum", version: "1.1.0", digest: "c".repeat(64),
    }, parameters: [...strategies[1].parameters,
      { key: "threshold", label: "涨幅阈值", type: "decimal", minimum: "0", maximum: "1", scale: 4, default: "0.0000" },
      { key: "enabled", label: "允许做多", type: "boolean", default: true },
      { key: "comparison", label: "阈值比较", type: "enum", choices: [{ value: "strict", label: "大于" }, { value: "inclusive", label: "大于等于" }], default: "strict" },
    ] };
    listedStrategies = [...strategies, installedStrategy];
    await page.getByRole("button", { name: "刷新策略", exact: true }).click();
    await expect(page.getByRole("combobox", { name: "策略", exact: true })
      .locator("option", { hasText: "外部动量" })).toHaveCount(1);
    await page.getByRole("combobox", { name: "策略", exact: true })
      .selectOption({ label: "外部动量 · 1.1.0" });
    listedStrategies = strategies;
    await page.getByRole("button", { name: "刷新策略", exact: true }).click();
    await expect(page.getByText("原策略未安装或实现已变化", { exact: false })).toBeVisible();

    await page
      .getByRole("combobox", { name: "策略", exact: true })
      .selectOption({ label: "双均线 · 1.0.0" });
    await page
      .getByRole("combobox", { name: "日线数据集", exact: true })
      .selectOption("dataset-1");
    await expect(
      page.getByRole("combobox", { name: "固定数据版本", exact: true }),
    ).toHaveValue(version.id);
    if (!local) {
      await page
        .getByRole("button", { name: "从合约资料创建", exact: true })
        .click();
      await page
        .getByRole("combobox", { name: "合约资料版本", exact: true })
        .selectOption("contracts-fixed");
      await page.getByLabel("规范合约 ID", {exact:true}).fill(rules.spec.contract.id);
      await page
        .getByRole("button", { name: "读取合约资料", exact: true })
        .click();
      await expect(page.getByLabel("来源映射预览")).toContainText(
        "建议研究乘数：10",
      );
      await expect(page.getByLabel("来源映射预览")).toContainText(
        "生效期间",
      );
      await page
        .getByRole("button", { name: "采用资料并补全规则", exact: true })
        .click();
      await expect(
        page.getByRole("textbox", { name: "合约乘数", exact: true }),
      ).toHaveValue("10");
      await expect(
        page.getByRole("textbox", { name: "最小变动价位", exact: true }),
      ).toHaveValue("");
      await expect(
        page.getByRole("textbox", { name: "规则开始日期", exact: true }),
      ).toHaveValue("");
      await page.getByRole("combobox", {name:"交易时间版本", exact:true}).selectOption(timeVersion.id);
      for (const [name, value] of [
        ["规则名称", "用户研究规则"],
        ["规则来源与依据", "离线测试"],
        ["合约乘数", "10"],
        ["最小变动价位", "1"],
        ["规则开始日期", "2024-01-01"],
        ["规则结束日期", "2024-12-31"],
        ["保证金比例（0—1）", "0.1"],
        ["开仓费用", "2"],
        ["平仓费用", "3"],
      ])
        await page
          .getByRole(
            name.includes("日期")
              ? "textbox"
              : ["保证金比例（0—1）", "开仓费用", "平仓费用"].includes(name)
                ? "spinbutton"
                : "textbox",
            { name, exact: true },
          )
          .fill(value);
      await page
        .getByRole("button", { name: "保存规则版本", exact: true })
        .click();
      await expect(page.getByLabel("固定合约规则")).toContainText(
        "用户研究规则",
      );
      const original = await page
        .getByRole("combobox", { name: "固定规则版本", exact: true })
        .inputValue();
      await page
        .getByRole("button", { name: "复制并修改规则", exact: true })
        .click();
      await page
        .getByRole("textbox", { name: "规则名称", exact: true })
        .fill("调整后的规则");
      await page
        .getByRole("spinbutton", { name: "开仓费用", exact: true })
        .fill("4");
      await page.getByText("从结算参数填写此期间", { exact: true }).click();
      await page
        .getByRole("combobox", { name: "结算参数版本", exact: true })
        .selectOption("settlement-fixed");
      await page
        .getByRole("textbox", { name: "参数交易日", exact: true })
        .fill("2023-12-29");
      await page
        .getByRole("button", { name: "读取结算参数", exact: true })
        .click();
      await expect(page.getByLabel("结算参数确认")).toContainText("0.050");
      const adopt = page.getByRole("button", {
        name: "确认并采用此期间",
        exact: true,
      });
      await expect(adopt).toBeDisabled();
      await page
        .getByRole("combobox", { name: "手续费原值单位", exact: true })
        .selectOption("permille");
      await page
        .getByRole("combobox", { name: "买投机保证金原值单位", exact: true })
        .selectOption("percent");
      await page
        .getByRole("textbox", { name: "假设生效开始", exact: true })
        .fill("2024-01-01");
      await page
        .getByRole("textbox", { name: "假设生效结束", exact: true })
        .fill("2024-12-31");
      await page
        .getByRole("textbox", {
          name: "单位、开平仓与期间确认依据",
          exact: true,
        })
        .fill("离线验证的明确研究假设");
      await expect(adopt).toBeDisabled();
      await page.getByRole("checkbox", { name: /我确认上述单位/ }).check();
      await adopt.click();
      await expect(
        page.getByRole("spinbutton", { name: "开仓费用", exact: true }),
      ).toHaveValue("0.00005");
      await expect(
        page.getByRole("spinbutton", {
          name: "保证金比例（0—1）",
          exact: true,
        }),
      ).toHaveValue("0.12");
      await page
        .getByRole("button", { name: "保存规则版本", exact: true })
        .click();
      await expect(page.getByLabel("固定合约规则")).toContainText(
        "调整后的规则",
      );
      await page
        .getByRole("combobox", { name: "固定规则版本", exact: true })
        .selectOption(original);
      await expect(page.getByLabel("固定合约规则")).toContainText(
        "用户研究规则",
      );
    }
    if (local) {
      await page.getByRole("button", {name:"新建规则", exact:true}).click();
      await page.getByRole("button", {name:"保存规则版本", exact:true}).click();
      await expect(page.getByRole("alert")).toContainText("请选择实际合约身份");
      await page.getByLabel("规则合约目录", {exact:true}).selectOption("rule-catalog");
      await page.getByLabel("规则合约", {exact:true}).selectOption(rules.spec.contract.id);
      await expect(page.getByText("交割月 2024-05", {exact:false})).toBeVisible();
      await page.getByRole("button", {name:"取消编辑", exact:true}).click();
    }
    await selectRules(page);
    await expect(
      page.getByRole("button", { name: "运行回测", exact: true }),
    ).toBeDisabled();
    await page.getByRole("checkbox", { name: /我接受历史收盘/ }).check();
    const runButton = page.getByRole("button", {
      name: "运行回测",
      exact: true,
    });
    await expect(runButton).toBeDisabled();
    if (local) await fillReferences();
    await page
      .getByRole("button", { name: "核对所选版本覆盖", exact: true })
      .click();
    await expect(runButton).toBeEnabled();
    if (local)
      await expect(
        page.getByText(
          "文件数据缺口需修正后重新导入，再选择新版本核对。不会自动跨来源补齐。",
          { exact: true },
        ),
      ).toBeVisible();
    await page.getByLabel("结束日期", { exact: true }).fill("2024-03-02");
    await expect(runButton).toBeDisabled();
    if (local) await fillReferences();
    await page
      .getByRole("button", { name: "核对所选版本覆盖", exact: true })
      .click();
    await expect(page.getByText("存在缺失日线", { exact: true })).toBeVisible();
    await expect(runButton).toBeDisabled();
    await page
      .getByRole("combobox", { name: "数据完整性要求", exact: true })
      .selectOption("allow_incomplete");
    await expect(runButton).toBeDisabled();
    await page
      .getByLabel("探索模式原因", { exact: true })
      .fill("仅观察缺失日期的影响");
    await page.getByRole("checkbox", { name: /我接受历史收盘/ }).check();
    await expect(runButton).toBeEnabled();
    await page
      .getByRole("combobox", { name: "数据完整性要求", exact: true })
      .selectOption("require_complete");
    await page.getByLabel("结束日期", { exact: true }).fill("2024-03-01");
    if (local) await fillReferences();
    await page
      .getByRole("button", { name: "核对所选版本覆盖", exact: true })
      .click();
    await page.getByRole("checkbox", { name: /我接受历史收盘/ }).check();
    await page
      .getByRole("combobox", { name: "策略", exact: true })
      .selectOption({ label: "收盘动量 · 1.0.0" });
    await expect(page.getByLabel("快均线周期", { exact: true })).toHaveCount(0);
    if (local) {
      listedStrategies = [...strategies, installedStrategy];
      await page.getByRole("button", { name: "刷新策略", exact: true }).click();
      await page.getByRole("combobox", { name: "策略", exact: true }).selectOption({ label: "外部动量 · 1.1.0" });
      await page.getByLabel("涨幅阈值", { exact: true }).fill("0.10001");
      await expect(page.getByText("涨幅阈值：请按声明", { exact: false })).toBeVisible();
      await expect(runButton).toBeDisabled();
      await page.getByLabel("涨幅阈值", { exact: true }).fill("0.1000");
      await page.getByLabel("允许做多", { exact: true }).uncheck();
      await page.getByLabel("阈值比较", { exact: true }).selectOption("inclusive");
    }
    await page.getByLabel("回看周期", { exact: true }).fill("3");
    await expect(runButton).toBeDisabled();
    await page.getByRole("checkbox", { name: /我接受历史收盘/ }).check();
    await runButton.click();
    await expect.poll(() => frozen?.strategy).toEqual(local ? installedStrategy.identity : strategies[1].identity);
    expect(frozen.parameters).toEqual(local ? { lookback: 3, threshold: "0.1000", enabled: false, comparison: "inclusive" } : { lookback: 3 });

    await expect(page.getByRole("img", { name: "回测权益曲线" })).toBeVisible();
    await expect(
      page.getByText("frozen-input-checksum", { exact: false }),
    ).toBeVisible();
    await expect(
      page.getByText("上一根日线信号", { exact: true }),
    ).toBeVisible();
    await page.getByText("每日结算与资金账", { exact: false }).click();
    await expect(page.getByRole("columnheader", { name: "结算余额 / 权益", exact: true })).toBeVisible();
    await expect(page.getByRole("columnheader", { name: "收盘估值差额", exact: true })).toBeVisible();
    await page.getByRole("button", { name: "按原输入重跑" }).click();
    await expect(
      page.getByRole("heading", { name: "运行 run-2", exact: true }),
    ).toBeVisible();
    await page
      .getByRole("combobox", { name: "对比运行", exact: true })
      .selectOption("run-1");
    await expect(page.getByText(/对比输入：SHFE.rb2405/)).toBeVisible();
    await page.getByText("导出结果与复现依据", { exact: true }).click();
    await expect(page.getByLabel("附带回测所用行情（OHLC）")).not.toBeChecked();
    let downloadEvent = page.waitForEvent("download");
    await page
      .getByRole("button", { name: "导出复现包 JSON", exact: true })
      .click();
    const downloaded = await downloadEvent;
    const stream = await downloaded.createReadStream();
    const chunks: Buffer[] = [];
    for await (const chunk of stream!) chunks.push(chunk);
    expect(Buffer.concat(chunks).toString()).toBe(exported);
    expect(exportFlags).toEqual(["false"]);
    await page.getByLabel("附带回测所用行情（OHLC）").check();
    downloadEvent = page.waitForEvent("download");
    await page
      .getByRole("button", { name: "导出复现包 JSON", exact: true })
      .click();
    await downloadEvent;
    expect(exportFlags).toEqual(["false", "true"]);
    downloadEvent = page.waitForEvent("download");
    await page
      .getByRole("button", { name: "导出结果 ZIP", exact: true })
      .click();
    expect((await downloadEvent).suggestedFilename()).toBe(
      "research-results.zip",
    );
    await page.getByText("导入复现包", { exact: true }).click();
    const upload = () =>
      page.getByLabel("选择复现包", { exact: true }).setInputFiles({
        name: "sample.asterion.json",
        mimeType: "application/json",
        buffer: Buffer.from(exported),
      });
    await upload();
    await expect(
      page.getByText("可复现：重新计算与原结果一致", { exact: true }),
    ).toBeVisible();
    expect(replayCount).toBe(0);
    const replayButton = page.getByRole("button", {
      name: "启动复现回测",
      exact: true,
    });
    await expect(replayButton).toBeDisabled();
    await page.getByLabel("按包内固定输入与模型假设重跑").check();
    await replayButton.click();
    await expect.poll(() => replayCount).toBe(1);
    for (const status of ["MISSING_DATA", "UNSUPPORTED", "INVALID"]) {
      packageStatus = status;
      await upload();
      await expect(replayButton).toBeDisabled();
      await expect(
        page.getByLabel("按包内固定输入与模型假设重跑"),
      ).toBeDisabled();
    }
    await page.getByText("参数批量实验", { exact: true }).click();
    const experiments = page.getByRole("region", { name: "参数实验" });
    await experiments.getByLabel("实验名称", { exact: true }).fill("动量参数实验");
    await experiments.getByLabel("变化：回看周期", { exact: true }).check();
    await experiments.getByRole("button", { name: "添加回看周期候选", exact: true }).click();
    await experiments.getByLabel("回看周期", { exact: true }).nth(1).fill("1");
    await page.getByRole("checkbox", { name: /我接受历史收盘/ }).check();
    await experiments.getByRole("button", { name: "提交参数实验", exact: true }).click();
    await expect.poll(() => experimentInput?.grid).toEqual({ lookback: [3, 1] });
    expect(experimentInput.base.strategy).toEqual(frozen.strategy);
    const table = experiments.getByRole("table", { name: "实验结果对比" });
    await expect(table).toContainText("-54");
    await experiments.getByRole("button", { name: "取消未完成组合", exact: true }).click();
    await expect(table).toContainText("已取消");
    await expect(table).toContainText("已完成");
    const validation = page.getByRole("region", { name: "后续区间验证" });
    await expect(validation.getByRole("button", { name: "冻结选择并验证" })).toBeDisabled();
    await page.getByLabel("开始日期", { exact: true }).fill("2024-04-01");
    await page.getByLabel("结束日期", { exact: true }).fill("2024-04-10");
    await page.getByRole("combobox", { name: "数据完整性要求", exact: true }).selectOption("allow_incomplete");
    await page.getByPlaceholder("说明为何接受未核验或不完整数据").fill("浏览器验证固定日期配置");
    await expect(validation.getByRole("button", { name: "冻结选择并验证" })).toBeDisabled();
    if (local) await fillReferences();
    await page.getByRole("button", { name: "核对所选版本覆盖", exact: true }).click();
    await page.getByRole("checkbox", { name: /我接受历史收盘/ }).check();
    await validation.getByRole("combobox", { name: "冻结组合", exact: true }).selectOption("combination-1");
    await validation.getByLabel("选择依据", { exact: true }).fill("冻结研究期选择，检查后续表现");
    await validation.getByRole("button", { name: "冻结选择并验证" }).click();
    await expect(validation.getByRole("table", { name: "研究与验证对比" })).toContainText("12");
    await expect(validation).toContainText("2024-04-05");
    await expect(validation.getByRole("button", { name: "冻结选择并验证" })).toHaveCount(0);
    await page.screenshot({ path: "../../.state/research-workbench.png" });
  });
}
