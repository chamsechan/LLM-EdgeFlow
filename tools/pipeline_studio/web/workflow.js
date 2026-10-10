// 运行快照描述提交时的草稿，不受之后的编辑或保存影响。
export function captureRun({ documentVersion, pipeline, filename, profile }) {
  return { documentVersion, pipeline: JSON.stringify(pipeline), filename, profile, startedAt: new Date().toISOString(), job: { status: "starting" }, id: "" };
}

export function runIsCurrent(run, { documentVersion, pipeline, profile, pending }) {
  return Boolean(run && run.documentVersion === documentVersion && !pending &&
    run.pipeline === JSON.stringify(pipeline) && run.profile === profile);
}

export function runSummary(job) {
  const labels = { starting: "正在提交", queued: "等待运行", running: "正在运行", completed: "运行已完成", failed: "运行失败", cancelled: "已取消" };
  const lines = [labels[job.status] || job.status];
  const summary = job.result?.["summary.json"];
  if (summary && typeof summary === "object") {
    // 计数来自 Demo 的样本状态，而非进程退出码。
    if (Number.isFinite(summary.total_samples)) lines.push(`样本 ${summary.total_samples} 条`);
    if (Number.isFinite(summary.success_count)) lines.push(`成功 ${summary.success_count} 条`);
    if (Number.isFinite(summary.failed_count)) lines.push(`失败 ${summary.failed_count} 条`);
  }
  if (job.error) lines.push(job.error.message || String(job.error.code || "运行错误"));
  if (job.status === "completed") lines.push("请核对样本输出；业务效果尚需验收。");
  return lines.join("\n");
}

export function renderSamples(container, result) {
  container.replaceChildren();
  const records = result?.["results.jsonl"];
  if (!Array.isArray(records)) return;
  for (const [index, record] of records.slice(0, 50).entries()) {
    const card = document.createElement("details"); card.className = "run-sample"; card.open = index === 0;
    const heading = document.createElement("summary");
    heading.textContent = `行 ${index} · 状态 ${record.status ?? "未知"}`;
    card.append(heading);
    const fields = document.createElement("dl");
    const output = record.output;
    const entries = output && typeof output === "object" && !Array.isArray(output) ? Object.entries(output) : [["输出", output ?? "无输出"]];
    for (const [key, value] of entries) {
      const term = document.createElement("dt"); term.textContent = key;
      const description = document.createElement("dd"); description.textContent = typeof value === "string" ? value : JSON.stringify(value, null, 2);
      fields.append(term, description);
    }
    card.append(fields); container.append(card);
  }
  if (records.length > 50) {
    const hint = document.createElement("p"); hint.textContent = `展示前 50 / ${records.length} 条样本，完整内容见原始结果 JSON。`;
    container.append(hint);
  }
}
