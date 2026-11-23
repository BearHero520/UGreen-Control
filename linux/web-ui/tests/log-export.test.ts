import assert from 'node:assert/strict';
import { test } from 'node:test';

test('authenticated export downloads a validated ZIP and reports missing sources', async () => {
  let captured: any; let clicked = false; let blob: Blob | undefined; let removed = false;
  Object.assign(globalThis, {
    document: { baseURI: 'https://nas.test/ugreen-toolbox/', body: { append: () => {} },
      createElement: () => ({ click: () => { clicked = true; }, remove: () => { removed = true; } }) },
    window: { crypto: { randomUUID: () => 'fixture' } },
    fetch: async (url: string, options: RequestInit) => {
      captured = [url, options];
      return new Response(JSON.stringify({ ok: true, redacted: true, mime: 'application/zip',
        filename: 'ugreen-toolbox-logs-20261030T080000Z.zip', data_base64: btoa('PK\x03\x04fixture'), incomplete: true }));
    },
    setTimeout: (callback: () => void) => { callback(); return 0; },
  });
  URL.createObjectURL = (value: Blob) => { blob = value; return 'blob:fixture'; };
  URL.revokeObjectURL = () => {};
  const { downloadLogs } = await import('../src/lib/logExport.ts');
  assert.equal(await downloadLogs(), true);
  assert.equal(captured[0], '/ugreen-toolbox/api/logs/export');
  assert.equal(captured[1].method, 'POST');
  assert.equal(captured[1].body, '');
  assert.equal(captured[1].credentials, 'same-origin');
  assert.equal(captured[1].headers['X-UGREEN-Request'], '1');
  assert.equal(await blob!.text(), 'PK\x03\x04fixture');
  assert.ok(clicked && removed);
  for (const invalid of [
    { redacted: false }, { filename: '../../secrets.zip' }, { mime: 'text/html' }, { data_base64: btoa('unsafe') },
  ]) {
    clicked = false;
    globalThis.fetch = async () => new Response(JSON.stringify({ redacted: true, mime: 'application/zip',
      filename: 'ugreen-toolbox-logs-20261030T080000Z.zip', data_base64: btoa('PK\x03\x04fixture'), ...invalid }));
    await assert.rejects(downloadLogs());
    assert.equal(clicked, false);
  }
});
