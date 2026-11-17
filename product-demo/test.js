const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { mkdtemp, rm } = require('node:fs/promises');
const { tmpdir } = require('node:os');
const path = require('node:path');
const net = require('node:net');
const http = require('node:http');
const { once } = require('node:events');
const { createApp } = require('./server');
const { command, parse } = require('./redis');
test('RESP handles split UTF-8 bulk and arrays', () => {
  const wire = Buffer.from('$6\r\n한글\r\n');
  assert.equal(parse(wire.subarray(0, 7)), null);
  assert.equal(parse(wire).value, '한글');
  assert.deepEqual(parse(Buffer.from('*2\r\n$1\r\na\r\n:3\r\n')).value, ['a', 3]);
});
test('real C Redis: product lifecycle, TTL, isolation, validation and outage', async () => {
  const probe = net.createServer(); probe.listen(0, '127.0.0.1'); await once(probe, 'listening');
  const port = probe.address().port; await new Promise(resolve => probe.close(resolve));
  const directory = await mkdtemp(path.join(tmpdir(), 'redis-market-'));
  const child = spawn(path.resolve(__dirname, '../c-mini-redis/mini-redis'), ['--bind','127.0.0.1','--port',String(port),'--dir',directory]);
  const origin = new Map();
  const dbms = http.createServer(async (req, res) => {
    let raw = ''; for await (const chunk of req) raw += chunk;
    const { sql } = JSON.parse(raw);
    let output = '', ok = true;
    const select = sql.match(/SELECT (.+) FROM products WHERE id = (\d+);/i);
    const insert = sql.match(/INSERT INTO products \(id, name, category, price, kind, color, cache_ttl\) VALUES \((\d+), '((?:''|[^'])*)', '([^']*)', (\d+), '([^']*)', '([^']*)', (\d+)\);/i);
    if (select) {
      const product = origin.get(select[2]);
      if (product) output = select[1].trim().toLowerCase() === 'id' ? `${product.id}\n` : `${product.id},${product.name},${product.category},${product.price},${product.kind},${product.color},${product.cacheTtl}\n`;
    } else if (insert) {
      if (origin.has(insert[1])) { ok = false; }
      else origin.set(insert[1], { id: insert[1], name: insert[2].replaceAll("''", "'"), category: insert[3], price: Number(insert[4]), kind: insert[5], color: insert[6], cacheTtl: Number(insert[7]) });
    } else { ok = false; }
    res.writeHead(ok ? 200 : 400, { 'Content-Type':'application/json' });
    res.end(JSON.stringify(ok ? {ok:true,output} : {ok:false,error:'SQL error'}));
  });
  dbms.listen(0, '127.0.0.1'); await once(dbms, 'listening');
  let app;
  try {
    for (let i = 0; ; i++) {
      try { await command(['PING'], {port}); break; } catch (error) { if(i > 40) throw error; await new Promise(r => setTimeout(r, 50)); }
    }
    app = createApp({port, dbmsUrl:`http://127.0.0.1:${dbms.address().port}/query`}); app.listen(0,'127.0.0.1'); await once(app, 'listening');
    const base = `http://127.0.0.1:${app.address().port}`;
    async function request(route, method = 'GET', value) {
      const response = await fetch(base + route, {method, headers: {'Content-Type':'application/json'}, body: value === undefined ? undefined : JSON.stringify(value)});
      return { status: response.status, ...await response.json() };
    }
    assert.equal((await fetch(base)).status, 200);
    let state = await request('/api/state'); assert.equal(state.products.length, 0);
    assert.equal((await request('/api/seed','POST',{ttl:60})).inserted, 4);
    assert.equal((await request('/api/products/1001')).source, 'dbms');
    assert.equal((await request('/api/products/1001')).hit, true);
    assert.equal((await request('/api/products/9999')).status, 404);
    assert.equal((await request('/api/products/1005','POST',{name:'한글 상품 <script>',price:12300,ttl:1})).saved, true);
    assert.equal((await request('/api/products/1005')).product.name, '한글 상품 <script>');
    await command(['SET','unrelated','keep','EX',60],{port});
    state = await request('/api/state'); assert.equal(state.products.length,2); assert.equal(state.stats.requests,4);
    assert.ok(state.products.find(p => p.id === '1005').ttl >= 0);
    assert.equal((await request('/api/products/1005','POST',{name:'bad',price:-1,ttl:500})).status,400);
    await new Promise(r => setTimeout(r,1200));
    assert.equal((await request('/api/products/1005')).hit,false);
    assert.equal((await request('/api/products/1005')).product.name, '한글 상품 <script>');
    assert.equal((await request('/api/products/1001','DELETE')).deleted,1);
    assert.equal((await request('/api/products/1001')).source, 'dbms');
    assert.equal(await command(['GET','unrelated'],{port}),'keep');
    state = await request('/api/state'); assert.equal(state.stats.hits,2); assert.equal(state.stats.misses,5); assert.equal(state.products.length,2);
    const comparison = await request('/api/products/1001?compare=1');
    assert.equal(comparison.comparison.id, '1001');
    assert.ok(comparison.comparison.dbmsMs >= 0);
    state = await request('/api/state'); assert.equal(state.comparisons.length, 1);
    assert.equal(state.stats.hits, 3); assert.equal(state.stats.misses, 6);
    const blocked = await fetch(base+'/api/seed',{method:'POST',headers:{Origin:'http://other.example'}}); assert.equal(blocked.status,403);
    child.kill('SIGTERM'); await once(child,'exit');
    assert.equal((await request('/api/state')).status,503);
  } finally {
    if(app) { app.closeAllConnections(); await new Promise(resolve => app.close(resolve)); }
    dbms.closeAllConnections(); await new Promise(resolve => dbms.close(resolve));
    if(child.exitCode === null && child.signalCode === null) { child.kill('SIGTERM'); await once(child,'exit'); }
    await rm(directory,{recursive:true,force:true});
  }
});

test('DBMS SQL endpoint forwards one SQL request and preserves result and errors', async () => {
  const received = [];
  const dbms = http.createServer(async (req, res) => {
    let body = '';
    for await (const chunk of req) body += chunk;
    const request = JSON.parse(body);
    received.push(request.sql);
    const failure = request.sql.includes('INVALID');
    res.writeHead(failure ? 400 : 200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(failure
      ? { ok: false, error: 'SQL syntax error' }
      : { ok: true, output: '1,Mina,24\n' }));
  });
  dbms.listen(0, '127.0.0.1');
  await once(dbms, 'listening');
  const app = createApp({ dbmsUrl: 'http://127.0.0.1:' + dbms.address().port + '/query' });
  app.listen(0, '127.0.0.1');
  await once(app, 'listening');
  const base = 'http://127.0.0.1:' + app.address().port;
  try {
    const page = await fetch(base);
    assert.equal(page.status, 200);
    assert.match(await page.text(), /Mini DBMS SQL 콘솔/);
    assert.equal((await fetch(base + '/dbms.css')).status, 200);
    const invalidBody = await fetch(base + '/api/dbms/query', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: 'null'
    });
    assert.equal(invalidBody.status, 400);
    const success = await fetch(base + '/api/dbms/query', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sql: 'SELECT id, name, age FROM people WHERE id = 1;' })
    });
    assert.equal(success.status, 200);
    assert.deepEqual(await success.json(), { ok: true, output: '1,Mina,24\n' });
    const failure = await fetch(base + '/api/dbms/query', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sql: 'INVALID;' })
    });
    assert.equal(failure.status, 400);
    assert.deepEqual(await failure.json(), { ok: false, error: 'SQL syntax error' });
    assert.deepEqual(received, [
      'SELECT id, name, age FROM people WHERE id = 1;',
      'INVALID;'
    ]);
  } finally {
    app.closeAllConnections();
    await new Promise(resolve => app.close(resolve));
    dbms.closeAllConnections();
    await new Promise(resolve => dbms.close(resolve));
  }
});
