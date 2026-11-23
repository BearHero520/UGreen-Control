import { api } from './api.ts';

type LogExport = { redacted: boolean; filename: string; mime: string; data_base64: string; incomplete: boolean };

export async function downloadLogs() {
  const data = await api<LogExport>('/logs/export', { method: 'POST', body: '' });
  if (data.redacted !== true || data.mime !== 'application/zip' ||
      !/^ugreen-toolbox-logs-\d{8}T\d{6}Z\.zip$/.test(data.filename) ||
      typeof data.data_base64 !== 'string' || data.data_base64.length > 7 * 1024 * 1024) {
    throw new Error('日志包格式或脱敏标记无效');
  }
  const raw = atob(data.data_base64);
  if (!raw.startsWith('PK\x03\x04')) throw new Error('日志包内容无效');
  const bytes = Uint8Array.from(raw, (value) => value.charCodeAt(0));
  const url = URL.createObjectURL(new Blob([bytes], { type: data.mime }));
  const link = document.createElement('a');
  try {
    link.href = url; link.download = data.filename;
    document.body.append(link); link.click();
  } finally {
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 30_000);
  }
  return data.incomplete;
}
