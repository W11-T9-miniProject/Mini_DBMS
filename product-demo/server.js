const http = require('node:http');
const fs = require('node:fs/promises');
const path = require('node:path');
const { performance } = require('node:perf_hooks');
const { command } = require('./redis');
const prefix = 'product-demo:product:';
const samples = [
  { id: '1001', name: 'Studio Headphones', category: 'AUDIO', price: 129000, kind: 'headphones', color: 'sand' },
  { id: '1002', name: 'Everyday Keyboard', category: 'DESK', price: 89000, kind: 'keyboard', color: 'mint' },
  { id: '1003', name: 'Pocket Speaker', category: 'AUDIO', price: 59000, kind: 'speaker', color: 'lavender' },
  { id: '1004', name: 'Arc Desk Lamp', category: 'LIVING', price: 45000, kind: 'lamp', color: 'peach' }
];
function createApp(config = {}) {
  const stats = { hits: 0, misses: 0, requests: 0, totalMs: 0 };
  const logs = [];
  const comparisons = [];
  const run = args => command(args, config);
  const record = (cmd, result, ms) => logs.unshift({ time: Date.now(), command: cmd, result, ms: +ms.toFixed(2) });
  function json(res, status, data) {
    res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify(data));
  }
  async function queryDbms(sql) {
    const dbmsUrl = config.dbmsUrl || process.env.DBMS_URL ||
      'http://127.0.0.1:' + (process.env.DBMS_PORT || 8080) + '/query';
    let upstream;
    try {
      upstream = await fetch(dbmsUrl, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ sql }), signal: AbortSignal.timeout(7000)
      });
    } catch {
      throw Object.assign(Error('Mini DBMS API에 연결할 수 없습니다. 서버와 포트를 확인하세요.'), { status: 503 });
    }
    let result;
    try { result = await upstream.json(); }
    catch { throw Object.assign(Error('Mini DBMS API 응답이 올바른 JSON이 아닙니다.'), { status: 502 }); }
    if (!result || typeof result.ok !== 'boolean' ||
        (result.ok && typeof result.output !== 'string') ||
        (!result.ok && typeof result.error !== 'string')) {
      throw Object.assign(Error('Mini DBMS API 응답 형식이 올바르지 않습니다.'), { status: 502 });
    }
    if (!upstream.ok || !result.ok) throw Object.assign(Error(result.error || 'Mini DBMS SQL 오류'), { status: upstream.status || 400 });
    return result.output;
  }
  function csvRow(line) {
    const fields = []; let field = '', quoted = false;
    for (let i = 0; i < line.length; i++) {
      const ch = line[i];
      if (ch === '"' && quoted && line[i + 1] === '"') { field += '"'; i++; }
      else if (ch === '"') quoted = !quoted;
      else if (ch === ',' && !quoted) { fields.push(field); field = ''; }
      else field += ch;
    }
    if (quoted) throw Object.assign(Error('Mini DBMS 상품 결과 CSV가 올바르지 않습니다.'), { status: 502 });
    fields.push(field); return fields;
  }
  async function productFromDbms(id) {
    const output = await queryDbms(`SELECT id, name, category, price, kind, color, cache_ttl FROM products WHERE id = ${id};`);
    if (!output.trim()) return null;
    const fields = csvRow(output.trim().split(/\r?\n/, 1)[0]);
    if (fields.length !== 7 || fields[0] !== id || !/^\d+$/.test(fields[3]) || !/^\d+$/.test(fields[6])) {
      throw Object.assign(Error('products 테이블 결과가 예상한 스키마와 다릅니다.'), { status: 502 });
    }
    const cacheTtl = Number(fields[6]);
    if (!Number.isInteger(cacheTtl) || cacheTtl < 1 || cacheTtl > 300) throw Object.assign(Error('상품 cache_ttl은 1~300초여야 합니다.'), { status: 502 });
    return { id: fields[0], name: fields[1], category: fields[2], price: Number(fields[3]), kind: fields[4], color: fields[5], cacheTtl };
  }
  async function fetchProduct(id) {
    const key = prefix + id;
    const start = performance.now();
    const raw = await run(['GET', key]);
    const redisMs = performance.now() - start;
    if (raw !== null) return { hit: true, source: 'redis', product: JSON.parse(raw), key, redisMs, dbmsMs: 0, ms: redisMs };
    const dbmsStart = performance.now();
    const product = await productFromDbms(id);
    const dbmsMs = performance.now() - dbmsStart;
    if (!product) return { hit: false, source: 'none', product: null, key, redisMs, dbmsMs, ms: redisMs + dbmsMs };
    await save(product, product.cacheTtl);
    return { hit: false, source: 'dbms', product, key, redisMs, dbmsMs, ms: redisMs + dbmsMs };
  }
  async function body(req) {
    let input = '';
    for await (const chunk of req) {
      input += chunk;
      if (Buffer.byteLength(input) > 8192) throw Object.assign(Error('입력 크기 초과'), { status: 413 });
    }
    try { return JSON.parse(input || '{}'); } catch { throw Object.assign(Error('JSON 형식 오류'), { status: 400 }); }
  }
  function validate(value) {
    if (!/^[a-zA-Z0-9_-]{1,40}$/.test(value.id || '') || typeof value.name !== 'string' || !value.name.trim() || value.name.length > 80 || !Number.isSafeInteger(value.price) || value.price < 0 || !Number.isInteger(value.ttl) || value.ttl < 1 || value.ttl > 300) {
      throw Object.assign(Error('ID, 상품명, 0 이상의 정수 가격, TTL 1~300초를 확인하세요.'), { status: 400 });
    }
  }
  async function save(product, ttl) {
    const start = performance.now();
    await run(['SET', prefix + product.id, JSON.stringify(product), 'EX', ttl]);
    record(`SET ${prefix + product.id} <상품 JSON> EX ${ttl}`, 'OK', performance.now() - start);
  }
  return http.createServer(async (req, res) => {
    const url = new URL(req.url, 'http://localhost');
    try {
      if (url.pathname === '/api/dbms/query' && req.method === 'POST') {
        const payload = await body(req);
        const sql = payload && typeof payload === 'object' && !Array.isArray(payload) ? payload.sql : null;
        if (typeof sql !== 'string' || !sql.trim() || Buffer.byteLength(sql, 'utf8') > 4096 || sql.includes('\0')) {
          return json(res, 400, { ok: false, error: 'SQL 문장을 4KiB 이내로 입력하세요.' });
        }
        try { return json(res, 200, { ok: true, output: await queryDbms(sql) }); }
        catch (error) { return json(res, error.status || 502, { ok: false, error: error.message }); }
      }
      // Only same-origin browser writes are accepted. No arbitrary Redis commands are exposed.
      if (['POST', 'DELETE'].includes(req.method) && req.headers.origin && req.headers.origin !== `http://${req.headers.host}`) return json(res, 403, { error: '다른 출처의 요청은 허용하지 않습니다.' });
      if (url.pathname === '/api/state' && req.method === 'GET') {
        await run(['PING']);
        const keys = (await run(['KEYS', '*'])).filter(key => key.startsWith(prefix));
        const products = (await Promise.all(keys.map(async key => {
          const raw = await run(['GET', key]);
          const ttl = await run(['TTL', key]);
          if (raw === null || ttl === -2) return null;
          try { return { ...JSON.parse(raw), key, ttl }; } catch { return null; }
        }))).filter(Boolean).sort((a,b) => a.id.localeCompare(b.id));
        return json(res, 200, { connected: true, products, stats, logs: logs.slice(0, 80), comparisons: comparisons.slice(0, 20) });
      }
      if (url.pathname === '/api/seed' && req.method === 'POST') {
        const { ttl = 120 } = await body(req);
        if (!Number.isInteger(ttl) || ttl < 1 || ttl > 300) return json(res, 400, { error: 'TTL은 1~300초입니다.' });
        const inserted = [];
        for (const product of samples) {
          const existing = await queryDbms(`SELECT id FROM products WHERE id = ${product.id};`);
          if (!existing.trim()) {
            const values = [product.id, `'${product.name}'`, `'${product.category}'`, String(product.price), `'${product.kind}'`, `'${product.color}'`, String(ttl)];
            await queryDbms(`INSERT INTO products (id, name, category, price, kind, color, cache_ttl) VALUES (${values.join(', ')});`);
            inserted.push(product.id);
          }
          await run(['DEL', prefix + product.id]);
        }
        for (const id of inserted) record(`DBMS INSERT products #${id}`, 'OK', 0);
        record(`DEL ${prefix}1001…1004`, 'OK', 0);
        logs.splice(80);
        return json(res, 200, { saved: samples.length, inserted: inserted.length, source: 'dbms' });
      }
      const match = url.pathname.match(/^\/api\/products\/([a-zA-Z0-9_-]{1,40})$/);
      if (match) {
        const key = prefix + match[1];
        if (req.method === 'POST' && !/^\d{1,18}$/.test(match[1])) return json(res, 400, { error: 'DBMS 상품 ID는 1~18자리 숫자여야 합니다.' });
        if (req.method === 'POST') {
          const value = { ...await body(req), id: match[1] }; validate(value);
          const { id, name, price } = value;
          const product = { id, name: name.trim(), price, category: 'CUSTOM', kind: 'box', color: 'sand', cacheTtl: value.ttl };
          const quote = text => `'${text.replaceAll("'", "''")}'`;
          await queryDbms(`INSERT INTO products (id, name, category, price, kind, color, cache_ttl) VALUES (${id}, ${quote(product.name)}, 'CUSTOM', ${price}, 'box', 'sand', ${product.cacheTtl});`);
          await run(['DEL', key]);
          record(`DBMS INSERT products #${id}`, 'OK', 0); record(`DEL ${key}`, '1', 0); logs.splice(80);
          return json(res, 200, { saved: true, source: 'dbms' });
        }
        if (req.method === 'GET' && url.searchParams.get('compare') === '1') {
          if (!/^\d{1,18}$/.test(match[1])) return json(res, 400, { error: 'DBMS 상품 ID는 숫자여야 합니다.' });
          await run(['DEL', key]);
          const cold = await fetchProduct(match[1]);
          if (!cold.product) return json(res, 404, { error: `ID ${match[1]} 상품이 DBMS 원본에 없습니다.` });
          const warm = await fetchProduct(match[1]);
          stats.requests += 2; stats.misses++; stats.hits++; stats.totalMs += cold.ms + warm.ms;
          comparisons.unshift({ time: Date.now(), id: match[1], dbmsMs: +cold.dbmsMs.toFixed(2), coldTotalMs: +cold.ms.toFixed(2), redisMs: +warm.redisMs.toFixed(2), speedup: warm.ms > 0 ? +(cold.ms / warm.ms).toFixed(2) : null });
          comparisons.splice(20);
          record(`GET ${key}`, 'MISS → DBMS → SET', cold.ms); record(`GET ${key}`, 'HIT', warm.ms); logs.splice(80);
          return json(res, 200, { comparison: comparisons[0], product: cold.product });
        }
        if (req.method === 'GET') {
          if (!/^\d{1,18}$/.test(match[1])) return json(res, 400, { error: 'DBMS 상품 ID는 숫자여야 합니다.' });
          const result = await fetchProduct(match[1]);
          stats.requests++; stats[result.hit ? 'hits' : 'misses']++; stats.totalMs += result.ms;
          record(`GET ${key}`, result.hit ? 'HIT' : result.product ? 'MISS → DBMS → SET' : 'MISS · 원본 없음', result.ms); logs.splice(80);
          return json(res, result.product ? 200 : 404, { ...result, ms: +result.ms.toFixed(2) });
        }
        if (req.method === 'DELETE') {
          const start = performance.now(); const deleted = await run(['DEL', key]);
          record(`DEL ${key}`, String(deleted), performance.now() - start); logs.splice(80);
          return json(res, 200, { deleted, sourcePreserved: true });
        }
      }
      const file = { '/': 'index.html', '/app.js': 'app.js', '/style.css': 'style.css', '/dbms.css': 'dbms.css' }[url.pathname];
      if (!file || req.method !== 'GET') return json(res, 404, { error: '찾을 수 없습니다.' });
      const data = await fs.readFile(path.join(__dirname, 'public', file));
      res.writeHead(200, { 'Content-Type': file.endsWith('.html') ? 'text/html; charset=utf-8' : file.endsWith('.css') ? 'text/css' : 'text/javascript', 'Cache-Control': 'no-store' }); res.end(data);
    } catch (error) { json(res, error.status || 503, { error: error.status ? error.message : `Redis 요청 실패: ${error.message}`, connected: false }); }
  });
}
if (require.main === module) createApp({ host: process.env.REDIS_HOST, port: Number(process.env.REDIS_PORT || 16380) }).listen(Number(process.env.WEB_PORT || 3000), '127.0.0.1', () => console.log(`상품 조회: http://127.0.0.1:${process.env.WEB_PORT || 3000}`));
module.exports = { createApp };
