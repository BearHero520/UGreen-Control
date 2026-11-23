// Preserve the DSM app proxy prefix; the standalone container still uses /api.
const API = new URL('api', document.baseURI).pathname;

export type ApiOptions = {
  method?: 'GET' | 'POST';
  query?: string;
  body?: string;
  signal?: AbortSignal;
};

export type ApiError = Error & {
  requestId?: string;
  status?: number;
  path?: string;
};

function createRequestId() {
  if (window.crypto?.randomUUID) return window.crypto.randomUUID();
  return `react-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

export async function api<T>(path: string, options: ApiOptions = {}): Promise<T> {
  const requestId = createRequestId();
  const headers: Record<string, string> = { 'X-Request-ID': requestId, 'X-UGREEN-Request': '1' };
  if (options.body !== undefined) headers['Content-Type'] = 'text/plain';

  const response = await fetch(`${API}${path}${options.query ? `?${options.query}` : ''}`, {
    method: options.method ?? 'GET',
    headers,
    body: options.body,
    credentials: 'same-origin',
    cache: 'no-store',
    signal: options.signal,
  });
  if (response.status === 401 && response.headers.get('X-UGREEN-Login') === 'required') {
    window.location.reload();
  }
  const text = await response.text();
  let data: T & { ok?: boolean; error?: string };
  try {
    data = JSON.parse(text) as T & { ok?: boolean; error?: string };
  } catch {
    const error = new Error(`服务器返回异常响应（HTTP ${response.status}）`) as ApiError;
    error.requestId = requestId;
    error.status = response.status;
    error.path = path;
    throw error;
  }

  if (!response.ok || data.ok === false) {
    const error = new Error(data.error || `请求失败（HTTP ${response.status}）`) as ApiError;
    error.requestId = response.headers.get('X-Request-ID') || requestId;
    error.status = response.status;
    error.path = path;
    throw error;
  }
  return data;
}
