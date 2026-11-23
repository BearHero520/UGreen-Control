import assert from 'node:assert/strict';
import { test } from 'node:test';
import fs from 'node:fs';
import vm from 'node:vm';

test('API requests stay under the DSM application prefix and keep CSRF headers', async () => {
  for (const [baseURI, expected] of [['https://nas.test:5001/ugreen-toolbox/', '/ugreen-toolbox/api/settings'], ['http://127.0.0.1:5088/', '/api/settings']]) {
    let captured: [string, RequestInit] | undefined;
    Object.assign(globalThis, {
      document: { baseURI }, window: { crypto: { randomUUID: () => 'fixture' } },
      fetch: async (url: string, options: RequestInit) => { captured = [url, options]; return new Response('{"ok":true}'); },
    });
    const { api } = await import(`../src/lib/api.ts?base=${encodeURIComponent(baseURI)}`);
    await api('/settings', { method: 'POST', body: 'mode.global=off' });
    assert.equal(captured?.[0], expected);
    assert.equal(captured?.[1].credentials, 'same-origin');
    assert.equal((captured?.[1].headers as Record<string, string>)['X-UGREEN-Request'], '1');
  }
});

test('DSM shell opens a same-origin iframe in a resizable application window', () => {
  let definition: { render: (h: Function) => any; beforeDestroy: () => void } | undefined;
  const sandbox: any = { SYNO: { SDS: { App: { UGreenToolbox: {} } }, namespace: () => {} },
    Vue: { extend: (value: any) => { definition = value; } }, window: { location: { protocol: 'https:' } }, fetch: () => Promise.resolve() };
  vm.runInNewContext(fs.readFileSync(new URL('../../synology/ui/UGreenToolbox.js', import.meta.url), 'utf8'), sandbox);
  const h = (tag: string, options: any, children: any) => ({ tag, options, children });
  const app = definition!.render(h);
  assert.equal(app.tag, 'v-app-instance');
  assert.equal(app.children[0].tag, 'v-app-window');
  assert.equal(app.children[0].options.props.resizable, true);
  assert.equal(app.children[0].children[0].tag, 'iframe');
  assert.equal(app.children[0].children[0].options.attrs.src, '/ugreen-toolbox/');
  sandbox.window.location.protocol = 'http:';
  assert.equal(definition!.render(h).children[0].children[0].tag, 'p');
});
