const fragment = new URLSearchParams(location.hash.slice(1));
export const initialPipeline = fragment.get("pipeline") || "";

export async function api(path, options = {}) {
  const { allowFalse = false, ...fetchOptions } = options;
  const headers = { "Content-Type": "application/json", ...(options.headers || {}) };
  // Keep the page's directory prefix when Studio is opened through a port proxy.
  const url = new URL(`api/v1${path}`, location.href);
  let response;
  try { response = await fetch(url, { ...fetchOptions, headers }); }
  catch (error) {
    throw new Error(`接口 ${url.pathname}${url.search} 请求失败：${error.message}。请确认 Studio 服务仍在运行及页面地址正确。`);
  }
  const text = await response.text();
  let payload;
  try {
    payload = JSON.parse(text);
  } catch {
    const detail = text.replace(/\s+/g, " ").trim().slice(0, 200) || "空响应";
    const failure = new Error(`接口 ${url.pathname}${url.search} 返回非 JSON 响应（HTTP ${response.status}）：${detail}。请检查 Studio 服务和端口转发地址。`);
    failure.status = response.status;
    throw failure;
  }
  if (!response.ok || (payload.ok === false && !allowFalse)) {
    const error = payload.error || payload.diagnostics || payload;
    const detail = typeof error?.message === "string" ? error.message : JSON.stringify(error);
    const failure = new Error(`接口 ${url.pathname}${url.search}（HTTP ${response.status}${error?.code ? ` · ${error.code}` : ""}）：${detail}`);
    failure.payload = payload;
    failure.status = response.status;
    throw failure;
  }
  return payload;
}

export function write(path, method, body, allowFalse = false) {
  return api(path, { method, body: JSON.stringify(body), allowFalse });
}
