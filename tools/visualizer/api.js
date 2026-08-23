const fragment = new URLSearchParams(location.hash.slice(1));
export const initialPipeline = fragment.get("pipeline") || "";

export async function api(path, options = {}) {
  const { allowFalse = false, ...fetchOptions } = options;
  const headers = { "Content-Type": "application/json", ...(options.headers || {}) };
  const response = await fetch(`/api/v1${path}`, { ...fetchOptions, headers });
  const payload = await response.json();
  if (!response.ok || (payload.ok === false && !allowFalse)) {
    const error = payload.error || payload.diagnostics || payload;
    const failure = new Error(typeof error?.message === "string" ? error.message : JSON.stringify(error));
    failure.payload = payload;
    failure.status = response.status;
    throw failure;
  }
  return payload;
}

export function write(path, method, body, allowFalse = false) {
  return api(path, { method, body: JSON.stringify(body), allowFalse });
}
