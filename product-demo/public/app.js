const $ = selector => document.querySelector(selector);
const escapeHtml = value => String(value).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const money = value => new Intl.NumberFormat('ko-KR').format(value);
let toastTimer, refreshing = false, lastProducts = '', lastLogs = '';
function toast(message) { $('#toast').textContent = message; $('#toast').classList.add('show'); clearTimeout(toastTimer); toastTimer = setTimeout(() => $('#toast').classList.remove('show'), 3500); }
async function api(url, options) {
  const response = await fetch(url, { ...options, headers: { 'Content-Type': 'application/json' } });
  const data = await response.json();
  if (!response.ok) throw Error(data.error || '요청에 실패했습니다.');
  return data;
}
function artwork(kind) {
  const drawings = {
    headphones: '<path d="M39 67V52a36 36 0 0172 0v15" fill="none" stroke="#53665e" stroke-width="12"/><path d="M40 49a35 35 0 0170 0" fill="none" stroke="#809087" stroke-width="5"/><rect x="30" y="55" width="24" height="39" rx="10" fill="#40564e"/><rect x="96" y="55" width="24" height="39" rx="10" fill="#40564e"/><rect x="34" y="59" width="13" height="29" rx="6" fill="#7c8d82"/><rect x="103" y="59" width="13" height="29" rx="6" fill="#7c8d82"/>',
    keyboard: '<g transform="translate(6 20) rotate(-9 70 40)"><rect x="10" y="17" width="126" height="64" rx="7" fill="#b8c9bf"/><rect x="10" y="12" width="126" height="63" rx="7" fill="#e0e9e1"/>' + Array.from({length:4},(_,r)=>Array.from({length:11},(_,c)=>`<rect x="${16+c*10.5}" y="${19+r*12}" width="8" height="9" rx="1.8" fill="${c===10?'#819f8d':'#f9fcf6'}"/>`).join('')).join('') + '</g>',
    speaker: '<rect x="44" y="16" width="65" height="84" rx="17" fill="#8e87a2"/><rect x="44" y="13" width="65" height="80" rx="17" fill="#b1a9c7"/><circle cx="76" cy="54" r="24" fill="#9991ae"/><circle cx="76" cy="54" r="18" fill="#817992"/><circle cx="76" cy="54" r="13" fill="#a69dbb"/><circle cx="70" cy="24" r="2" fill="#ddd6ec"/><circle cx="80" cy="24" r="2" fill="#ddd6ec"/><path d="M65 85h22" stroke="#d1c8e2" stroke-width="2"/>',
    lamp: '<ellipse cx="77" cy="101" rx="30" ry="6" fill="#c39d78"/><path d="M77 96V53q0-26 24-26" fill="none" stroke="#b58a65" stroke-width="6"/><path d="M90 18q24-3 30 26H81q0-17 9-26" fill="#dbb896"/><ellipse cx="100" cy="44" rx="19" ry="4" fill="#f2d7ad"/>',
    box: '<path d="M40 35l35-17 35 17v46L75 100 40 81z" fill="#cfbea5"/><path d="M40 35l35 18 35-18-35-17z" fill="#e1d5c3"/><path d="M75 53v47M57 27l36 17v17" fill="none" stroke="#b2a18b" stroke-width="3"/>'
  };
  return `<svg viewBox="0 0 150 120" aria-hidden="true">${drawings[kind] || drawings.box}</svg>`;
}
async function refresh() {
  if (refreshing) return;
  refreshing = true;
  try {
    const data = await api('/api/state');
    $('#connection').textContent = '● C Mini Redis 연결됨'; $('#connection').classList.remove('offline');
    const { stats, products, logs, comparisons = [] } = data;
    $('#requests').innerHTML = `${stats.requests}<small>requests</small>`;
    $('#rate').textContent = stats.requests ? `${Math.round(stats.hits / stats.requests * 100)}%` : '—';
    $('#counts').textContent = `HIT ${stats.hits} / MISS ${stats.misses}`;
    $('#latency').innerHTML = `${stats.requests ? (stats.totalMs / stats.requests).toFixed(2) : '—'}<small>ms</small>`;
    $('#keys').innerHTML = `${products.length}<small>keys</small>`; $('#product-count').textContent = products.length;
    const productHtml = products.length ? products.map(p => `<article class="product"><div class="art ${['mint','lavender','peach'].includes(p.color) ? p.color : 'sand'}"><span class="product-id">#${escapeHtml(p.id)}</span><button class="delete" data-delete="${escapeHtml(p.id)}" aria-label="${escapeHtml(p.name)} 캐시 제거" title="Redis 캐시만 제거">×</button>${artwork(p.kind)}</div><div class="product-info"><span class="category">${escapeHtml(p.category)}</span><button class="product-name" data-get="${escapeHtml(p.id)}">${escapeHtml(p.name)}</button><span class="product-price">₩${money(p.price)}</span><div class="product-bottom"><span class="ttl ${p.ttl <= 10 ? 'low' : ''}">◷ ${p.ttl < 0 ? '만료 없음' : p.ttl+'s 남음'}</span><button class="get-button" data-get="${escapeHtml(p.id)}">조회하기 ↗</button></div></div></article>`).join('') : '<div class="empty"><div class="empty-icon">▱</div><b>Redis 캐시가 비어 있습니다</b>샘플을 DBMS에 등록한 뒤 상품을 조회하면 캐시가 채워집니다.<br>캐시가 만료되어도 원본 상품은 DBMS에 남아 있습니다.</div>';
    if (productHtml !== lastProducts) { $('#products').innerHTML = productHtml; lastProducts = productHtml; }
    const logHtml = logs.map(log => `<div class="log"><div class="log-top"><span>${new Date(log.time).toLocaleTimeString('ko-KR', {hour12:false})}</span><span class="${log.result.toLowerCase()}">${escapeHtml(log.result)} · ${log.ms.toFixed(2)}ms</span></div><code>${escapeHtml(log.command)}</code></div>`).join('') || '<div class="logs-empty">명령을 실행하면 로그가 표시됩니다.</div>';
    if (lastLogs !== logHtml) { $('#logs').innerHTML = logHtml; lastLogs = logHtml; }
    $('#log-count').textContent = logs.length;
    $('#comparisons').innerHTML = comparisons.length ? comparisons.map(item => `<tr><td>${new Date(item.time).toLocaleTimeString('ko-KR',{hour12:false})}</td><td>${escapeHtml(item.id)}</td><td>${item.dbmsMs.toFixed(2)} ms</td><td>${item.coldTotalMs.toFixed(2)} ms</td><td>${item.redisMs.toFixed(2)} ms</td><td>${item.speedup === null ? '—' : item.speedup.toFixed(2)+'×'}</td></tr>`).join('') : '<tr><td colspan="6">비교 실행을 누르면 결과가 기록됩니다.</td></tr>';
  } catch (error) {
    $('#connection').textContent = '● Redis 연결 끊김'; $('#connection').classList.add('offline');
    lastProducts = '';
    $('#products').innerHTML = '<div class="empty"><b>Redis에 연결할 수 없습니다</b>서버 실행 상태를 확인하세요.<br>기존 데이터와 지표는 마지막 수신 시점의 값입니다.</div>';
  } finally { refreshing = false; }
}
async function lookup(id) {
  try {
    const data = await api(`/api/products/${encodeURIComponent(id)}`);
    $('#query-id').value = id;
    $('#result').className = `result ${data.hit ? 'hit' : 'miss'}`;
    const route = data.hit ? 'CACHE HIT · Redis에서 반환' : 'CACHE MISS · DBMS에서 읽어 Redis에 저장';
    $('#result').innerHTML = `<span>${data.hit ? '✓' : '↻'}</span><div><b>${route} <span class="code-label">${data.ms.toFixed(2)} ms</span></b><p>${escapeHtml(data.key)} · DBMS ${data.dbmsMs.toFixed(2)} ms / Redis ${data.redisMs.toFixed(2)} ms</p><pre>${escapeHtml(JSON.stringify(data.product,null,2))}</pre></div>`;
    $('#flow-outcome').textContent = data.hit ? 'HIT → Redis에서 상품 반환' : 'MISS → DBMS SELECT → Redis SET';
    $('#flow-outcome').className = `flow-outcome ${data.hit ? 'hit' : 'miss'}`;
    $('#dbms-flow-outcome').textContent = data.hit ? '이번 조회는 Redis HIT · 원본 호출 생략' : '원본에서 읽은 상품을 Redis에 저장';
    $('#dbms-dot').classList.add('connected');
    $('#redis-node').classList.remove('pulse'); void $('#redis-node').offsetWidth; $('#redis-node').classList.add('pulse');
    await refresh();
  } catch (error) { toast(error.message); $('#flow-outcome').textContent = '조회 오류 · DBMS / Redis 연결 확인'; $('#result').className = 'result miss'; $('#result').textContent = error.message; await refresh(); }
}
$('#query-form').addEventListener('submit', event => { event.preventDefault(); lookup($('#query-id').value.trim()); });
$('#compare').addEventListener('click', async () => {
  const id = $('#query-id').value.trim(); const button = $('#compare'); button.disabled = true;
  try {
    const data = await api(`/api/products/${encodeURIComponent(id)}?compare=1`);
    $('#result').className = 'result hit';
    $('#result').innerHTML = `<span>↔</span><div><b>비교 기록 완료 · ${escapeHtml(id)}번 상품</b><p>DBMS 경유 ${data.comparison.coldTotalMs.toFixed(2)} ms → Redis 재조회 ${data.comparison.redisMs.toFixed(2)} ms</p><pre>${escapeHtml(JSON.stringify(data.product,null,2))}</pre></div>`;
    $('#flow-outcome').textContent = '비교 완료 → MISS 조회 후 재조회 HIT'; $('#flow-outcome').className = 'flow-outcome hit';
    $('#dbms-flow-outcome').textContent = `DBMS ${data.comparison.dbmsMs.toFixed(2)} ms · Redis HIT ${data.comparison.redisMs.toFixed(2)} ms`;
    $('#dbms-dot').classList.add('connected'); await refresh();
  } catch (error) { toast(error.message); $('#result').className = 'result miss'; $('#result').textContent = error.message; }
  finally { button.disabled = false; }
});
$('#download-comparison').addEventListener('click', async () => {
  try {
    const { comparisons } = await api('/api/state');
    if (!comparisons.length) return toast('먼저 전후 비교를 실행하세요.');
    const rows = [['time','product_id','dbms_ms','cold_path_ms','redis_hit_ms','speedup'], ...comparisons.map(x => [new Date(x.time).toISOString(),x.id,x.dbmsMs,x.coldTotalMs,x.redisMs,x.speedup ?? ''])];
    const csv = rows.map(row => row.map(value => `"${String(value).replaceAll('"','""')}"`).join(',')).join('\n');
    const link = document.createElement('a'); link.href = URL.createObjectURL(new Blob(['\ufeff'+csv],{type:'text/csv;charset=utf-8'})); link.download = 'redis-cache-comparison.csv'; link.click(); URL.revokeObjectURL(link.href);
  } catch (error) { toast(error.message); }
});
$('#products').addEventListener('click', async event => {
  const get = event.target.closest('[data-get]'); if (get) return lookup(get.dataset.get);
  const del = event.target.closest('[data-delete]');
  if (del) { try { await api(`/api/products/${encodeURIComponent(del.dataset.delete)}`, {method:'DELETE'}); toast('Redis에서 상품을 삭제했습니다.'); await refresh(); } catch (error) { toast(error.message); } }
});
$('#seed').addEventListener('click', async () => {
  const button = $('#seed'); button.disabled = true;
  try {
    const result = await api('/api/seed', {method:'POST', body:JSON.stringify({})});
    for (const id of ['1001','1002','1003','1004']) await api(`/api/products/${id}`);
    $('#flow-outcome').textContent = '샘플 MISS → DBMS 조회 → Redis 캐시 저장';
    $('#flow-outcome').className = 'flow-outcome hit';
    $('#dbms-flow-outcome').textContent = 'DBMS 원본 샘플 4개를 확인하고 캐시에 채웠습니다';
    $('#dbms-dot').classList.add('connected');
    toast(`샘플 ${result.saved}개를 확인해 상품 목록에 표시했습니다 · 신규 원본 ${result.inserted}개`);
    await refresh();
  } catch (error) { toast(error.message); } finally { button.disabled = false; }
});
$('#save-form').addEventListener('submit', async event => {
  event.preventDefault(); const button = event.submitter; button.disabled = true;
  const data = Object.fromEntries(new FormData(event.target)); data.price = Number(data.price); data.ttl = Number(data.ttl);
  try { await api(`/api/products/${encodeURIComponent(data.id)}`, { method:'POST', body:JSON.stringify(data) }); toast('상품을 DBMS 원본에 등록했습니다. 다음 조회에서 Redis 캐시가 채워집니다.'); await refresh(); } catch (error) { toast(error.message); } finally { button.disabled = false; }
});
$('#dbms-form').addEventListener('submit', async event => {
  event.preventDefault();
  const button = $('#dbms-submit');
  const sql = $('#dbms-sql').value;
  const started = performance.now();
  button.disabled = true;
  $('#dbms-status').textContent = '요청 중';
  $('#dbms-result').textContent = 'Mini DBMS API에 SQL을 보내는 중…';
  $('#dbms-flow-outcome').textContent = '웹 서버 → Mini DBMS API';
  try {
    const response = await fetch('/api/dbms/query', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sql })
    });
    const data = await response.json();
    const elapsed = performance.now() - started;
    $('#dbms-time').textContent = elapsed.toFixed(2) + ' ms';
    if (!response.ok || !data.ok) throw Object.assign(Error(data.error || 'SQL 실행에 실패했습니다.'), { status: response.status });
    $('#dbms-status').textContent = 'API 연결됨';
    $('#dbms-status').className = 'dbms-status online';
    $('#dbms-dot').classList.add('connected');
    $('#dbms-result').textContent = data.output || '(결과 행 없음)';
    $('#dbms-flow-outcome').textContent = 'SQL 실행 완료 · ' + elapsed.toFixed(2) + ' ms';
    $('#dbms-flow-outcome').className = 'flow-outcome hit';
    $('#dbms-node').classList.remove('pulse'); void $('#dbms-node').offsetWidth; $('#dbms-node').classList.add('pulse');
  } catch (error) {
    const elapsed = performance.now() - started;
    $('#dbms-time').textContent = elapsed.toFixed(2) + ' ms';
    const connected = error.status && error.status !== 503 && error.status !== 502;
    $('#dbms-status').textContent = connected ? 'SQL 오류 · API 연결됨' : 'API 연결 실패';
    $('#dbms-status').className = 'dbms-status ' + (connected ? 'online' : 'offline');
    $('#dbms-dot').classList.toggle('connected', Boolean(connected));
    $('#dbms-result').textContent = error.message;
    $('#dbms-flow-outcome').textContent = connected ? 'DBMS가 SQL 오류를 반환했습니다' : 'Mini DBMS 서버를 확인하세요';
    $('#dbms-flow-outcome').className = 'flow-outcome ' + (connected ? 'miss' : '');
  } finally { button.disabled = false; }
});
refresh(); setInterval(refresh, 1000);
