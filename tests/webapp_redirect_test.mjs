import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { runInNewContext } from 'node:vm';

const html = readFileSync(new URL('../webapp/webapp-redirect.html', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1]
  .replaceAll('{{leader}}', '192.168.29.69')
  .replaceAll('{{home}}', '192.168.29.62');
const target = 'http://192.168.29.69/index.html?home=192.168.29.62';

function openBootstrap(fetchResponse) {
  const navigations = [];
  const warnings = [];
  const status = { textContent: '' };
  let expire;
  const done = runInNewContext(script, {
    AbortController,
    fetch: (url, options) => {
      assert.equal(url, target);
      assert.equal(options.method, 'HEAD');
      assert.equal(options.mode, 'cors');
      return fetchResponse(options.signal);
    },
    setTimeout: (callback, delay) => {
      assert.ok(delay > 0 && delay <= 3000, 'Fallback must be bounded to three seconds');
      expire = callback;
      return 1;
    },
    clearTimeout: () => { expire = undefined; },
    document: { getElementById: () => status },
    window: { location: { replace: url => navigations.push(url) } },
    console: { warn: (...args) => warnings.push(args) },
  });
  return { done, navigations, warnings, status, expire: () => expire() };
}

function response(ok = true, type = 'text/html') {
  return { ok, headers: { get: () => type } };
}

test('reachable HTML app receives the original controller in home', async () => {
  const browser = openBootstrap(() => response(true, 'Text/HTML; charset=utf-8'));
  await browser.done;
  assert.deepEqual(browser.navigations, [target]);
  assert.equal(browser.warnings.length, 0);
});

test('HTTP errors and non-HTML pages use the local app without a loop', async () => {
  for (const result of [response(false), response(true, 'application/json'), response(true, null)]) {
    const browser = openBootstrap(() => result);
    await browser.done;
    assert.deepEqual(browser.navigations, ['/index.html']);
    assert.equal(browser.warnings.length, 1);
    assert.match(browser.status.textContent, /unavailable/);
  }
});

test('network and CORS errors fall back locally', async () => {
  const browser = openBootstrap(() => { throw new TypeError('Failed to fetch'); });
  await browser.done;
  assert.deepEqual(browser.navigations, ['/index.html']);
  assert.equal(browser.warnings.length, 1);
});

test('an unresponsive host is aborted and falls back locally', async () => {
  let signal;
  const browser = openBootstrap(value => {
    signal = value;
    return new Promise((resolve, reject) => {
      value.addEventListener('abort', () => reject(value.reason));
    });
  });
  assert.deepEqual(browser.navigations, []);
  browser.expire();
  await browser.done;
  assert.equal(signal.aborted, true);
  assert.deepEqual(browser.navigations, ['/index.html']);
  assert.equal(browser.warnings.length, 1);
});
