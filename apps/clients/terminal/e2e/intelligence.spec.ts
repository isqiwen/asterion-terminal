import { test, expect } from "./test";
import { rpc, seedHistory, taskRpc } from "./dataset-fixture";
import { openView } from "./navigation-helper";
import { resolve } from "node:path";

test("@journey human announcement review saves through real Data before Intelligence publication and survives offline replay", async ({
  page,
}, testInfo) => {
  test.setTimeout(120000);
  expect(process.env.ASTERION_TEST_NODE_ISOLATED).toBe("1");
  const build = process.env.ASTERION_CPP_BUILD ?? resolve(__dirname, "../../../../build/Debug");
  const seeded = await seedHistory(page.request, [100, 110], "notice-tables", {
    month: "2026-11",
    tablesOnly: true,
  });
  const path = resolve(
    build,
    "material-fixtures",
    "asterion_test_material_provider" + (process.platform === "darwin" ? ".dylib" : ".so"),
  );
  const candidate = (await rpc(page.request, "native.plugins.preview", { path })).plugin_candidate;
  await rpc(page.request, "native.plugins.install", { path, sha256: candidate.sha256 });
  const initial = await rpc(page.request, "runtime.snapshot");
  const service = initial.data.service;
  const plugins = initial.native_plugins.items
    .filter((p: { managed?: boolean; selectable: boolean }) => !p.managed && p.selectable)
    .map((p: { sha256: string }) => p.sha256);
  const stopped = await rpc(page.request, "node.action", { id: "local", service, action: "stop" });
  const declaration = stopped.nodes
    .find((n: { id: string }) => n.id === "local")
    .health.services.find((s: { id: string }) => s.id === service);
  await rpc(page.request, "node.plugins.configure", {
    id: "local",
    service,
    revision: declaration.revision,
    plugins: [...new Set([...plugins, candidate.sha256])],
  });
  await rpc(page.request, "node.data.local.open");
  const source = (await rpc(page.request, "data.material.sources")).material_sources.find(
    (s: { id: string }) => s.id === "fixture-material",
  );
  await rpc(page.request, "data.material.submit", {
    id: "browser-notice-material",
    source_id: source.id,
    resource_id: "margin-announcement",
    use_scope: "Synthetic post-hoc research",
    provider_artifact: source.provider_artifact,
    max_requests: 1,
    max_bytes: 4096,
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "data.material.task", { id: "browser-notice-material" }))
          .material_task.state,
    )
    .toBe("succeeded");
  await page.goto("/");
  await openView(page, "materials");
  const materials = page.getByRole("region", { name: "情报材料", exact: true });
  await materials.getByLabel("材料操作 ID", { exact: true }).fill("browser-notice-material");
  await materials.getByRole("button", { name: "读取固定材料", exact: true }).click();
  await materials.getByRole("button", { name: "人工核验此材料", exact: true }).click();
  const panel = page.getByRole("region", { name: "人工公告核验", exact: true });
  await panel.getByRole("button", { name: "打开本机情报服务", exact: true }).click();
  await expect(panel).toContainText("local/intelligence");
  await panel.getByRole("button", { name: "读取固定材料", exact: true }).click();
  const original = panel.getByLabel("材料原文", { exact: true });
  const body = "SHFE/rb/2026-11 计划于2026-10-24将保证金率调整为12%。";
  await expect(original).toHaveValue(body);
  for (const [quote, label] of [
    ["SHFE/rb/2026-11", "合约主体"],
    ["保证金率", "参数名称"],
    ["2026-10-24", "计划生效日"],
    ["12%", "百分比"],
    ["计划", "计划语气"],
  ]) {
    await original.evaluate((element, selected) => {
      const el = element as HTMLTextAreaElement,
        at = el.value.indexOf(selected);
      if (at < 0) throw Error("missing synthetic quote");
      el.focus();
      el.setSelectionRange(at, at + selected.length);
      el.dispatchEvent(new Event("select", { bubbles: true }));
      el.dispatchEvent(new MouseEvent("mouseup", { bubbles: true }));
    }, quote);
    const use = panel.getByRole("button", { name: new RegExp("使用选中文本.*" + label) });
    await expect(use).toBeEnabled();
    await use.click();
  }
  await panel.getByRole("button", { name: "读取固定合约资料", exact: true }).click();
  await panel
    .getByRole("combobox", { name: "合约资料来源", exact: true })
    .selectOption(seeded.daily.contracts_source);
  await panel.getByRole("button", { name: "读取品种", exact: true }).click();
  await panel.getByRole("combobox", { name: "品种", exact: true }).selectOption("SHFE/rb");
  await panel.getByRole("button", { name: "读取合约", exact: true }).click();
  await panel.getByRole("combobox", { name: "合约", exact: true }).selectOption("SHFE/rb/2026-11");
  await panel.getByLabel("候选 ID", { exact: true }).fill("browser-notice");
  await panel.getByRole("button", { name: "提交固定候选", exact: true }).click();
  await expect(panel.getByRole("heading", { name: /browser-notice.*待人工核验/ })).toBeVisible();
  const fixed = (await rpc(page.request, "intelligence.get", { id: "browser-notice" }))
    .intelligence_record;
  expect(fixed.assessment.ready_for_human_review).toBe(true);
  expect(fixed.evidence.percentage.begin).toBe(
    Buffer.byteLength(body.slice(0, body.indexOf("12%")), "utf8"),
  );
  const approve = panel.getByRole("button", { name: "确认核验通过", exact: true });
  await expect(approve).toBeDisabled();
  await panel.getByLabel("核验人", { exact: true }).fill("Synthetic reviewer");
  await panel
    .getByLabel("核验理由", { exact: true })
    .fill("Exact original, units, planned context and scope reviewed.");
  const checks = panel.getByRole("checkbox");
  await expect(checks).toHaveCount(6);
  for (let i = 0; i < 5; ++i) await checks.nth(i).check();
  await expect(approve).toBeDisabled();
  await checks.nth(5).check();
  await approve.click();
  await expect(panel.getByRole("heading", { name: /browser-notice.*已核验待发布/ })).toBeVisible();
  await panel
    .getByLabel("发布理由", { exact: true })
    .fill("Publish synthetic planned notice for post-hoc research only.");
  await panel.getByRole("button", { name: "请求保存并发布", exact: true }).click();
  await expect(panel.getByRole("heading", { name: /browser-notice.*已发布/ })).toBeVisible();
  await expect(panel).toContainText("Data 保存时间");
  await expect(panel).toContainText("情报可用时间");
  const published = (await rpc(page.request, "intelligence.get", { id: "browser-notice" }))
    .intelligence_record;
  expect(published.published.content.kind).toBe("intelligence_content");
  expect(published.published.reference.kind).toBe("intelligence_publication");
  await panel.getByRole("button", { name: "交给 Research 审阅", exact: true }).click();
  const research = page.getByRole("region", { name: "期货研究", exact: true });
  await expect(research).toContainText("研究服务已连接");
  const cutoff = Number(published.published.available_at_ms) - 1;
  await research.getByLabel("公告审阅任务 ID", { exact: true }).fill("browser-notice-audit");
  await research
    .getByLabel("审阅截止时间（UTC）", { exact: true })
    .fill(new Date(cutoff).toISOString().slice(0, 23).replace(/0+$/, "").replace(/\.$/, ""));
  await research.getByRole("button", { name: "提交或确认原公告审阅", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (t: { id: string }) => t.id === "browser-notice-audit",
        )?.state,
    )
    .toBe("succeeded");
  await research.getByRole("button", { name: "查看结果", exact: true }).click();
  const report = research.getByRole("region", { name: "公告证据审阅结果", exact: true });
  await expect(report).toContainText("截至所选时点不可用");
  await expect(report).toContainText("不满足严格历史使用条件");
  await expect(report).toContainText("来源发布时间未知");
  await expect(report).toContainText("未验证预测有效性");
  const audit = (
    await taskRpc(page.request, "research", "task.result.read", { id: "browser-notice-audit" })
  ).task_result;
  expect(audit.kind).toBe("notice_review");
  expect(audit.result.source).toEqual(published.published.reference);
  expect(audit.result.as_of_ms).toBe(String(cutoff));
  expect(audit.result.published_by_cutoff).toBe(false);
  expect(audit.result.strict_history_eligible).toBe(false);
  await research.getByRole("button", { name: "重核并续办源引用", exact: true }).click();
  await expect(research).toContainText("源引用关系已确认");
  const sourceAck = (
    await taskRpc(page.request, "research", "notice.custody.reconcile", {
      id: "browser-notice-audit",
    })
  ).notice_custody;
  expect(sourceAck.phase).toBe("confirmed");
  expect(sourceAck.source).toEqual(published.published.reference);
  expect(sourceAck.consumer.version).toBe(audit.input_sha256);
  await openView(page, "eventSets");
  const sets = page.getByRole("region", { name: "固定事件集合", exact: true });
  await sets.getByRole("button", { name: "读取前32条候选", exact: true }).click();
  await sets.getByRole("checkbox").check({ timeout: 10000 });
  await sets.getByLabel("集合操作 ID", { exact: true }).fill("browser-fixed-event-set");
  await sets.getByLabel("集合名称", { exact: true }).fill("Synthetic explicit event set");
  await sets.getByLabel("使用范围", { exact: true }).fill(published.use_scope);
  await sets
    .getByLabel("集合截止时间（UTC）", { exact: true })
    .fill(
      new Date(Number(published.published.available_at_ms))
        .toISOString()
        .slice(0, 23)
        .replace(/0+$/, "")
        .replace(/\.$/, ""),
    );
  await sets
    .getByRole("combobox", { name: "集合研究模式", exact: true })
    .selectOption("strict_history");
  await sets.getByRole("button", { name: "预览固定选择与排除", exact: true }).click();
  await expect(sets).toContainText("source_publication_time_unknown");
  await expect(
    sets.getByRole("button", { name: "确认保存并发布集合", exact: true }),
  ).toBeDisabled();
  await sets
    .getByRole("combobox", { name: "集合研究模式", exact: true })
    .selectOption("post_hoc_research");
  await sets.getByRole("button", { name: "预览固定选择与排除", exact: true }).click();
  await expect(sets.getByRole("button", { name: "确认保存并发布集合", exact: true })).toBeEnabled();
  await sets.getByRole("button", { name: "确认保存并发布集合", exact: true }).click();
  await expect(sets).toContainText("集合已固定发布");
  const fixedSet = (
    await rpc(page.request, "intelligence.set.get", { id: "browser-fixed-event-set" })
  ).notice_set;
  expect(fixedSet.reference.kind).toBe("intelligence_set");
  expect(fixedSet.request.query.events).toEqual([published.event.reference]);
  expect(fixedSet.request.query.as_of_ms).toBe(published.published.available_at_ms);
  expect(fixedSet.members[0].publication).toEqual(published.published.reference);
  await sets.getByRole("button", { name: "交给 Research 集合审阅", exact: true }).click();
  await research.getByLabel("集合审阅任务 ID", { exact: true }).fill("browser-event-set-audit");
  await research.getByRole("button", { name: "提交或确认原集合审阅", exact: true }).click();
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).research_service.tasks.find(
          (task: { id: string }) => task.id === "browser-event-set-audit",
        )?.state,
    )
    .toBe("succeeded");
  await research.getByRole("button", { name: "查看结果", exact: true }).click();
  const setReport = research.getByRole("region", { name: "集合固定审阅结果", exact: true });
  await expect(setReport).toContainText(published.published.available_at_ms);
  await expect(setReport).toContainText("不满足严格实际历史资格");
  await expect(setReport).toContainText("集合组装时间不代表集合在历史截止时已存在。");
  const setAudit = (
    await taskRpc(page.request, "research", "task.result.read", { id: "browser-event-set-audit" })
  ).task_result;
  expect(setAudit.kind).toBe("event_set_review");
  expect(setAudit.result.source).toEqual(fixedSet.reference);
  expect(setAudit.result.events).toEqual(fixedSet.request.query.events);
  const setAck = (
    await taskRpc(page.request, "research", "notice.custody.reconcile", {
      id: "browser-event-set-audit",
    })
  ).notice_custody;
  expect(setAck.source).toEqual(fixedSet.reference);
  expect(setAck.consumer.kind).toBe("event_set_review");
  expect(setAck.consumer.version).toBe(setAudit.input_sha256);
  expect(setAck.phase).toBe("confirmed");
  await openView(page, "noticeReview");
  await research.getByRole("button", { name: "查看结果", exact: true }).click();
  await expect(report).toContainText("截至所选时点不可用");
  const subscriptionDefinition = {
    id: "browser-notice-reminders",
    name: "Synthetic application reminders",
    filter: {
      contracts: ["SHFE/rb/2026-11"],
      rule_id: "contract_margin_announcement",
      use_scope: published.use_scope,
      kinds: ["publication", "correction", "withdrawal"],
      include_followup_statements: false,
    },
    start_after: "0",
    consumer: "application_reminder",
  };
  const subscription = (
    await rpc(page.request, "intelligence.subscription.create", {
      definition: subscriptionDefinition,
    })
  ).notice_subscription;
  await rpc(page.request, "intelligence.subscription.control", {
    subscription: subscription.reference,
    revision: subscription.revision,
    active: false,
    gap_through: null,
  });
  // Real producer bytes come from the isolated fixture, never a live supplier.
  for (const [id, resource] of [
    ["browser-notice-correction-material", "margin-correction"],
    ["browser-notice-withdrawal-material", "margin-withdrawal"],
  ]) {
    await rpc(page.request, "data.material.submit", {
      id,
      source_id: source.id,
      resource_id: resource,
      use_scope: published.use_scope,
      provider_artifact: source.provider_artifact,
      max_requests: 1,
      max_bytes: 4096,
    });
    await expect
      .poll(async () => (await rpc(page.request, "data.material.task", { id })).material_task.state)
      .toBe("succeeded");
  }
  const revised = (
    await rpc(page.request, "data.material.read", { id: "browser-notice-correction-material" })
  ).material_read;
  expect(revised.text).toBe(body.replace("12%", "13%"));
  const replacementCandidate = {
    id: "browser-notice-replacement",
    material: revised.receipt.reference,
    dictionary: {
      snapshot: seeded.daily.snapshot,
      contracts_source: seeded.daily.contracts_source,
      exchange: "SHFE",
      product: "rb",
    },
    contract: fixed.contract,
    evidence: { ...fixed.evidence, percentage: { ...fixed.evidence.percentage, text: "13%" } },
    rule_id: fixed.rule_id,
    use_scope: fixed.use_scope,
    use_mode: fixed.use_mode,
    event: published.event.reference,
  };
  const replacementDraft = (
    await rpc(page.request, "intelligence.submit", { candidate: replacementCandidate })
  ).intelligence_record;
  await rpc(page.request, "intelligence.review", {
    id: "browser-replacement-review",
    candidate: replacementDraft.reference,
    operator_name: "Synthetic reviewer",
    reason: "Compared the corrected original and every literal field.",
    approve: true,
    checks: ["subject", "time", "quantity_unit", "modality_context", "source_use", "conflicts"],
  });
  const replacement = (
    await rpc(page.request, "intelligence.publish", {
      id: "browser-replacement-publication",
      candidate: replacementDraft.reference,
      data_owner: {
        node: revised.receipt.reference.node,
        service: revised.receipt.reference.service,
        domain: "data",
      },
      reason: "Publish a fixed replacement statement in the explicitly selected event.",
    })
  ).intelligence_record;
  expect(replacement.event).toEqual(published.event);
  await openView(page, "intelligence");
  await panel.getByLabel("候选 ID", { exact: true }).fill("browser-notice-replacement");
  await panel.getByRole("button", { name: "读取候选状态", exact: true }).click();
  const changes = panel.getByRole("region", { name: "事件更正与撤回", exact: true });
  await changes.getByRole("button", { name: "读取事件状态", exact: true }).click();
  const selectChangeEvidence = async (id: string) => {
    await changes.getByLabel("新证据材料 ID", { exact: true }).fill(id);
    await changes.getByRole("button", { name: "读取新证据", exact: true }).click();
    const original = changes.getByLabel("变化证据原文", { exact: true });
    await expect(original).not.toHaveValue("");
    await original.evaluate(element => {
      const field = element as HTMLTextAreaElement;
      field.focus();
      field.setSelectionRange(0, field.value.length);
      field.dispatchEvent(new Event("select", { bubbles: true }));
      field.dispatchEvent(new MouseEvent("mouseup", { bubbles: true }));
    });
  };
  await selectChangeEvidence("browser-notice-correction-material");
  await changes.getByLabel("核验人", { exact: true }).fill("Synthetic change reviewer");
  await changes
    .getByLabel("变化理由", { exact: true })
    .fill("New fixed original corrects the margin to 13%.");
  const append = changes.getByRole("button", { name: "保存并追加变化", exact: true });
  await expect(append).toBeDisabled();
  await changes.getByLabel("已核对新证据、事件归属和受影响引用", { exact: true }).check();
  await expect(append).toBeEnabled();
  await append.click();
  await expect(changes).toContainText("New fixed original corrects the margin to 13%.");
  const corrected = (
    await rpc(page.request, "intelligence.event.view", {
      event: published.event.reference,
      as_of_ms: null,
    })
  ).notice_event_view;
  expect(corrected.selected_publication).toEqual(replacement.published.reference);
  expect(corrected.latest_change.published.content.kind).toBe("intelligence_change_content");
  const affected = changes.getByRole("region", { name: "已登记研究引用", exact: true });
  await affected.getByRole("button", { name: "读取源引用登记", exact: true }).click();
  await expect(affected).toContainText("browser-notice-audit");
  await changes
    .getByLabel("截至时间（UTC 毫秒，留空查询当前）", { exact: true })
    .fill(published.published.available_at_ms);
  await changes.getByRole("button", { name: "读取事件状态", exact: true }).click();
  await expect(changes).toContainText("截至时点状态");
  await expect(append).toBeDisabled();
  const historical = (
    await rpc(page.request, "intelligence.event.view", {
      event: published.event.reference,
      as_of_ms: published.published.available_at_ms,
    })
  ).notice_event_view;
  expect(historical.selected_publication).toEqual(published.published.reference);
  await changes.getByLabel("截至时间（UTC 毫秒，留空查询当前）", { exact: true }).fill("");
  await changes.getByRole("button", { name: "读取事件状态", exact: true }).click();
  await changes
    .getByRole("combobox", { name: "变化动作", exact: true })
    .selectOption("withdraw", { timeout: 10000 });
  await selectChangeEvidence("browser-notice-withdrawal-material");
  await changes
    .getByLabel("变化理由", { exact: true })
    .fill("New original withdraws the corrected statement.");
  await changes.getByLabel("已核对新证据、事件归属和受影响引用", { exact: true }).check();
  await expect(append).toBeEnabled();
  await append.click();
  await expect(changes).toContainText("已撤回");
  const withdrawn = (
    await rpc(page.request, "intelligence.event.view", {
      event: published.event.reference,
      as_of_ms: null,
    })
  ).notice_event_view;
  expect(withdrawn.withdrawn).toBe(true);
  expect(withdrawn.latest_change.published.sequence).toBe("2");
  const tasksBeforeDelivery = (await rpc(page.request, "runtime.snapshot")).research_service.tasks;
  await openView(page, "subscriptions");
  const deliveries = page.getByRole("region", { name: "情报订阅与交付", exact: true });
  await page.reload();
  await openView(page, "subscriptions");
  await deliveries.getByRole("button", { name: "读取已保存订阅", exact: true }).click();
  const savedSubscriptions = deliveries.getByRole("region", { name: "已保存订阅", exact: true });
  await expect(savedSubscriptions).toContainText(subscriptionDefinition.name);
  await savedSubscriptions.getByRole("button", { name: "读取此订阅原条件", exact: true }).click();
  await expect(deliveries.getByLabel("订阅 ID", { exact: true })).toHaveValue(
    subscriptionDefinition.id,
  );
  await expect(deliveries.getByLabel("使用范围", { exact: true })).toHaveValue(published.use_scope);
  await deliveries.getByRole("button", { name: "核实订阅及断连范围", exact: true }).click();
  await expect(deliveries).toContainText("订阅已暂停");
  await deliveries.getByRole("button", { name: "恢复订阅并显示待补范围", exact: true }).click();
  await deliveries.getByRole("button", { name: "使用已观察到的待处理终点", exact: true }).click();
  await deliveries.getByLabel("交付操作 ID", { exact: true }).fill("browser-reminder-backfill");
  await expect(
    deliveries.getByRole("button", { name: "按明确范围提供交付", exact: true }),
  ).toBeDisabled();
  await deliveries.getByLabel("明确按历史补取交付", { exact: true }).check();
  await deliveries.getByRole("button", { name: "按明确范围提供交付", exact: true }).click();
  const delivered = deliveries.getByRole("region", { name: "原交付批次", exact: true });
  await expect(delivered).toContainText("历史补取批次");
  await deliveries.getByRole("button", { name: "读取已提供提醒记录", exact: true }).click();
  const reminders = deliveries.getByRole("region", { name: "应用内提醒记录", exact: true });
  await expect(reminders.getByRole("listitem")).toHaveCount(3);
  let lostConfirmation = false;
  await page.route("**/__asterion/api", async route => {
    const body = route.request().postData() ?? "";
    if (!lostConfirmation && body.includes("intelligence.subscription.ack")) {
      await route.fetch();
      lostConfirmation = true;
      await route.abort("failed");
    } else await route.continue();
  });
  await delivered.getByLabel("已核对本批覆盖范围与历史补取标签", { exact: true }).check();
  await delivered.getByRole("button", { name: "确认原交付范围", exact: true }).click();
  await expect.poll(() => lostConfirmation).toBe(true);
  await expect(deliveries).toContainText("当前结果待核实");
  await deliveries.getByRole("button", { name: "核实订阅及断连范围", exact: true }).click();
  await expect(delivered).toContainText("该原批次已确认");
  await expect(reminders.getByRole("listitem")).toHaveCount(3);
  const confirmedDelivery = (
    await rpc(page.request, "intelligence.subscription.get", {
      id: subscriptionDefinition.id,
    })
  ).notice_subscription;
  expect(confirmedDelivery.confirmed_through).toBe(confirmedDelivery.head);
  expect(confirmedDelivery.pending_delivery).toBeNull();
  expect((await rpc(page.request, "runtime.snapshot")).research_service.tasks).toEqual(
    tasksBeforeDelivery,
  );

  const unchanged = (
    await taskRpc(page.request, "research", "task.result.read", { id: "browser-notice-audit" })
  ).task_result;
  expect(unchanged.input_sha256).toBe(audit.input_sha256);
  expect(unchanged.result).toEqual(audit.result);
  await openView(page, "noticeReview");
  await research.getByRole("button", { name: "查看结果", exact: true }).click();
  await expect(report).toContainText("截至所选时点不可用");
  const sourceChanges = report.getByRole("region", { name: "固定来源的后继变化", exact: true });
  await sourceChanges.getByRole("button", { name: "核查来源后继变化", exact: true }).click();
  await expect(sourceChanges).toContainText("源事件当前已撤回");
  const sourceObserved = (
    await taskRpc(page.request, "research", "notice.source.inspect", {
      id: "browser-notice-audit",
    })
  ).notice_source_observation;
  expect(sourceObserved.consumer.version).toBe(audit.input_sha256);
  expect(sourceObserved.source).toEqual(published.published.reference);
  expect(sourceObserved.current.withdrawn).toBe(true);
  await expect(report).toContainText("截至所选时点不可用");
  await page.goto("/?screen=settings&category=resources");
  const catalogue = page.getByRole("region", { name: "业务资产目录", exact: true });
  await catalogue
    .getByRole("combobox", { name: "资产所属域", exact: true })
    .selectOption("intelligence");
  await catalogue
    .getByRole("combobox", { name: "资产类别", exact: true })
    .selectOption("intelligence_publication");
  await catalogue.getByRole("button", { name: "读取资产目录", exact: true }).click();
  await expect(catalogue).toContainText(published.published.reference.version);
  await expect(catalogue).toContainText("此目录不提供更改或删除");
  await expect(catalogue.getByRole("button", { name: "查看引用与维护" })).toHaveCount(0);
  await catalogue.getByRole("combobox", { name: "资产所属域", exact: true }).selectOption("data");
  await catalogue
    .getByRole("combobox", { name: "资产类别", exact: true })
    .selectOption("intelligence_content");
  await catalogue.getByRole("button", { name: "读取资产目录", exact: true }).click();
  await expect(catalogue).toContainText(published.published.content.version);
  await catalogue
    .getByRole("combobox", { name: "资产所属域", exact: true })
    .selectOption("research");
  await catalogue
    .getByRole("combobox", { name: "资产类别", exact: true })
    .selectOption("notice_review_result");
  await catalogue.getByRole("button", { name: "读取资产目录", exact: true }).click();
  await expect(catalogue).toContainText(audit.task.result_digest);
  await rpc(page.request, "node.action", {
    id: "local",
    service: "intelligence",
    action: "restart",
  });
  await expect
    .poll(
      async () =>
        (await rpc(page.request, "runtime.snapshot")).nodes
          .find((n: { id: string }) => n.id === "local")
          .health.services.find((s: { id: string }) => s.id === "intelligence").health,
      { timeout: 15000 },
    )
    .toBe("ready");
  await rpc(page.request, "node.action", { id: "local", service, action: "stop" });
  const replay = await rpc(page.request, "intelligence.publish", {
    id: published.publication_request.id,
    candidate: published.reference,
    data_owner: {
      node: published.material.reference.node,
      service: published.material.reference.service,
      domain: "data",
    },
    reason: published.publication_request.reason,
  });
  expect(replay.intelligence_record.published).toEqual(published.published);
  await page.goto("/");
  await openView(page, "intelligence");
  await panel.getByLabel("候选 ID", { exact: true }).fill("browser-notice");
  await panel.getByRole("button", { name: "读取候选状态", exact: true }).click();
  await expect(panel.getByRole("heading", { name: /browser-notice.*已发布/ })).toBeVisible();
  const references = panel.getByRole("region", { name: "已登记研究引用", exact: true });
  await references.getByRole("button", { name: "读取源引用登记", exact: true }).click();
  await expect(references).toContainText("browser-notice-audit");
  await expect(references).toContainText("引用关系已确认");
  await expect(references.getByRole("button", { name: /删除|解除/ })).toHaveCount(0);
  const registrations = (
    await rpc(page.request, "intelligence.consumers", {
      source: published.published.reference,
      after: "",
    })
  ).notice_consumers.items;
  expect(registrations).toHaveLength(1);
  expect(registrations[0].consumer.version).toBe(audit.input_sha256);
  expect(registrations[0].phase).toBe("confirmed");
  await rpc(page.request, "node.action", { id: "local", service: "intelligence", action: "stop" });
  const selected = (await rpc(page.request, "runtime.snapshot")).research_service;
  await rpc(page.request, "node.action", {
    id: selected.node_id,
    service: selected.service,
    action: "restart",
  });
  await rpc(page.request, "node.research.local.open");
  await taskRpc(page.request, "research", "notice.review.submit", {
    id: "browser-notice-audit",
    source: published.published.reference,
    as_of_ms: String(cutoff),
    use_scope: published.use_scope,
  });
  const restored = (
    await taskRpc(page.request, "research", "task.result.read", { id: "browser-notice-audit" })
  ).task_result;
  expect(restored.result).toEqual(audit.result);
  expect(restored.input_sha256).toBe(audit.input_sha256);
  await taskRpc(page.request, "research", "event.set.review.submit", {
    id: "browser-event-set-audit",
    source: fixedSet.reference,
    use_scope: published.use_scope,
  });
  const restoredSet = (
    await taskRpc(page.request, "research", "task.result.read", { id: "browser-event-set-audit" })
  ).task_result;
  expect(restoredSet.result).toEqual(setAudit.result);
  expect(restoredSet.input_sha256).toBe(setAudit.input_sha256);
  // Direct test RPC reselected Research; reload the UI's old connection snapshot.
  await page.reload();
  await openView(page, "noticeReview");
  await research.getByRole("button", { name: "查看结果", exact: true }).click();
  await expect(report).toContainText("截至所选时点不可用");
  await sourceChanges.getByRole("button", { name: "核查来源后继变化", exact: true }).click();
  await expect(sourceChanges).toContainText("来源当前状态未知");
  await expect(sourceChanges).not.toContainText("当前未见已发布后继变化");
  await expect(report).toContainText("截至所选时点不可用");
  await rpc(page.request, "node.action", {
    id: selected.node_id,
    service: selected.service,
    action: "stop",
  });
  const owners = (await rpc(page.request, "runtime.snapshot")).nodes
    .find((node: { id: string }) => node.id === "local")
    .health.services.filter((entry: { kind: string }) =>
      ["data", "intelligence", "research"].includes(entry.kind),
    );
  expect(new Set(owners.map((entry: { kind: string }) => entry.kind)).size).toBe(3);
  await expect
    .poll(async () => {
      const state = await rpc(page.request, "runtime.snapshot");
      return state.nodes
        .find((node: { id: string }) => node.id === "local")
        .health.services.filter((entry: { kind: string }) =>
          ["data", "intelligence", "research"].includes(entry.kind),
        )
        .every(
          (entry: { pid: number; desired_running: boolean }) =>
            !entry.pid && !entry.desired_running,
        );
    })
    .toBe(true);
  await page.goto("/?screen=settings&category=backup");
  const maintenance = page.getByRole("region", { name: "数据备份与恢复", exact: true });
  for (const entry of owners)
    await maintenance.getByRole("checkbox", { name: new RegExp(entry.id) }).check();
  const backup = testInfo.outputPath("notice-three-domain-backup");
  const recovery = testInfo.outputPath("notice-three-domain-restored");
  await maintenance.getByLabel("新备份目录", { exact: true }).fill(backup);
  await maintenance.getByRole("button", { name: "预览备份", exact: true }).click();
  await expect(maintenance.getByRole("button", { name: "保存备份", exact: true })).toBeDisabled();
  await maintenance.getByRole("checkbox", { name: "已核对所选范围、缺失依赖和新目录" }).check();
  await maintenance.getByRole("button", { name: "保存备份", exact: true }).click();
  await expect(maintenance).toContainText("备份已保存并核验");
  await maintenance.getByLabel("已有备份目录", { exact: true }).fill(backup);
  await maintenance.getByRole("button", { name: "核验备份", exact: true }).click();
  await maintenance.getByLabel("新恢复目录", { exact: true }).fill(recovery);
  await maintenance
    .getByRole("checkbox", {
      name: "确认恢复到独立新目录，不覆盖原数据或恢复授权",
    })
    .check();
  await maintenance.getByRole("button", { name: "恢复到新目录", exact: true }).click();
  await expect(maintenance).toContainText("已恢复到独立目录并核验，未启动服务");
  const verified = (await rpc(page.request, "system.backup.inspect", { backup: recovery })).backup;
  expect(verified.manifest.owners).toHaveLength(3);
  for (const dependency of verified.manifest.dependencies)
    if (
      [
        "intelligence_publication",
        "intelligence_content",
        "intelligence_preparation",
        "intelligence_program",
        "research_program",
        "notice_review",
        "intelligence_set",
        "intelligence_set_content",
        "event_set_review",
      ].includes(dependency.object.kind)
    )
      expect(dependency.included).toBe(true);
});
