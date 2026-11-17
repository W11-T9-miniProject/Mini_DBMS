(() => {
  "use strict";

  const byId = (id) => document.getElementById(id);
  const editor = byId("sql-editor");
  const HISTORY_KEY = "minidb-studio.history.v1";
  const PAGE_SIZE = 50;
  const presets = {
    all: "SELECT *\nFROM users\nLIMIT 20;",
    id: "SELECT *\nFROM users\nWHERE id = 1;",
    range: "SELECT id, name, age\nFROM users\nWHERE id BETWEEN 1 AND 10\nLIMIT 20;",
    scan: "SELECT *\nFROM users\nWHERE age = 25\nLIMIT 20;",
    insert: "INSERT INTO users (name, age, email)\nVALUES ('Jungle', 25, 'hello@mini.db');",
  };
  const sampleSQL = "INSERT INTO users (name, age, email)\nVALUES\n  ('김민수', 25, 'minsu@example.com'),\n  ('이서연', 28, 'seoyeon@example.com'),\n  ('박지훈', 30, 'jihoon@example.com'),\n  ('최유진', 25, 'yujin@example.com'),\n  ('정다은', 22, 'daeun@example.com');";
  const state = {
    token: "", busy: false, result: null, error: null, page: 0,
    history: readHistory(), connected: false, refreshing: false, refreshPending: false,
  };
  const numberFormat = new Intl.NumberFormat("ko-KR");
  let toastTimer;

  function node(tag, className, text) {
    const item = document.createElement(tag);
    if (className) item.className = className;
    if (text !== undefined) item.textContent = String(text);
    return item;
  }

  function readHistory() {
    try {
      const stored = JSON.parse(localStorage.getItem(HISTORY_KEY) || "[]");
      return Array.isArray(stored) ? stored.filter((item) =>
        item && typeof item.sql === "string" && item.sql.length <= 65536 &&
        Number.isFinite(item.at) && typeof item.ok === "boolean").slice(0, 20) : [];
    } catch {
      return [];
    }
  }

  function rememberQuery(sql, ok) {
    state.history.unshift({ sql: sql.slice(0, 65536), ok, at: Date.now() });
    state.history = state.history.slice(0, 20);
    try { localStorage.setItem(HISTORY_KEY, JSON.stringify(state.history)); } catch { /* Private browsing may disallow storage. */ }
    renderHistory();
  }

  function renderHistory() {
    const list = byId("history-list");
    list.replaceChildren();
    if (!state.history.length) {
      list.append(node("li", "history-empty", "실행한 쿼리가 여기에 남습니다."));
      return;
    }
    for (const item of state.history) {
      const li = node("li", "history-item" + (item.ok ? "" : " is-error"));
      const button = node("button");
      button.type = "button";
      button.title = item.sql;
      const summary = item.sql.replace(/\s+/g, " ").trim();
      button.setAttribute("aria-label", "쿼리 불러오기: " + summary);
      button.append(node("span", "history-state"), node("span", "history-query", summary),
        node("span", "history-time", new Date(item.at).toLocaleTimeString("ko-KR", { hour: "2-digit", minute: "2-digit", hour12: false })));
      button.addEventListener("click", () => {
        if (!state.busy) { setEditor(item.sql); showToast("최근 쿼리를 불러왔습니다. 실행 버튼을 눌러 실행하세요."); }
      });
      li.append(button);
      list.append(li);
    }
  }

  function showToast(message) {
    const toast = byId("toast");
    toast.textContent = message;
    toast.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { toast.hidden = true; }, 3800);
  }

  function firstKeyword(sql) {
    let remaining = sql.trimStart();
    while (remaining.startsWith("--") || remaining.startsWith("/*")) {
      if (remaining.startsWith("--")) {
        const end = remaining.indexOf("\n");
        remaining = end < 0 ? "" : remaining.slice(end + 1).trimStart();
      } else {
        const end = remaining.indexOf("*/", 2);
        remaining = end < 0 ? "" : remaining.slice(end + 2).trimStart();
      }
    }
    return (remaining.match(/^[a-z]+/i)?.[0] || "").toUpperCase();
  }

  function updateEditorView() {
    const text = editor.value;
    const output = byId("highlighted-code");
    const fragment = document.createDocumentFragment();
    const tokenPattern = /--[^\n]*|\/\*[\s\S]*?\*\/|'(?:''|[^'])*'|\b(?:SELECT|FROM|WHERE|INSERT|INTO|VALUES|LIMIT|EXPLAIN|BETWEEN|AND|NULL|AS|CREATE|UPDATE|DELETE|SET|ORDER|BY)\b|\b\d+\b/gi;
    let position = 0;
    for (const match of text.matchAll(tokenPattern)) {
      fragment.append(document.createTextNode(text.slice(position, match.index)));
      const token = match[0];
      const kind = token.startsWith("--") || token.startsWith("/*") ? "comment" :
        token.startsWith("'") ? "string" : /^\d/.test(token) ? "number" : "keyword";
      fragment.append(node("span", "sql-" + kind, token));
      position = match.index + token.length;
    }
    fragment.append(document.createTextNode(text.slice(position) + "\n"));
    output.replaceChildren(fragment);
    byId("line-numbers").textContent = text.split("\n").map((_line, index) => index + 1).join("\n") + "\n";
    byId("explain-query").disabled = state.busy || !["SELECT", "EXPLAIN"].includes(firstKeyword(text));
    syncEditorScroll();
  }

  function syncEditorScroll() {
    byId("code-highlight").scrollTop = editor.scrollTop;
    byId("code-highlight").scrollLeft = editor.scrollLeft;
    byId("line-numbers").scrollTop = editor.scrollTop;
  }

  function setEditor(sql) {
    if (state.busy) return;
    editor.value = sql;
    editor.scrollTop = editor.scrollLeft = 0;
    updateEditorView();
    editor.focus({ preventScroll: true });
  }

  function selectedSQL() {
    return editor.value.slice(editor.selectionStart, editor.selectionEnd).trim() || editor.value.trim();
  }

  function setBusy(busy) {
    state.busy = busy;
    document.body.classList.toggle("is-running", busy);
    editor.readOnly = busy;
    byId("run-query").disabled = busy;
    byId("run-query").querySelector("span").textContent = busy ? "실행 중…" : "쿼리 실행";
    byId("insert-sample").disabled = busy;
    byId("open-insert").disabled = busy;
    document.querySelectorAll("[data-preset]").forEach((button) => { button.disabled = busy; });
    document.querySelector(".results-panel").setAttribute("aria-busy", String(busy));
    updateEditorView();
  }

  class ApiError extends Error {
    constructor(status, code, message) { super(message); this.status = status; this.code = code; }
  }

  async function requestJSON(path, options = {}) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), options.method === "POST" ? 30000 : 8000);
    const headers = { Accept: "application/json" };
    if (state.token) headers.Authorization = "Bearer " + state.token;
    if (options.body) headers["Content-Type"] = "application/json";
    try {
      const response = await fetch(path, { ...options, headers, signal: controller.signal, cache: "no-store" });
      let payload;
      try { payload = await response.json(); }
      catch { throw new ApiError(response.status, "INVALID_RESPONSE", "서버가 JSON 응답을 보내지 않았습니다."); }
      if (!response.ok) {
        throw new ApiError(response.status, payload.error?.code || "HTTP_ERROR", payload.error?.message || "요청에 실패했습니다.");
      }
      return payload;
    } catch (error) {
      if (error instanceof ApiError) throw error;
      const message = error.name === "AbortError" ? "응답 대기 시간이 초과됐습니다." : "서버에 연결할 수 없습니다. API 서버가 실행 중인지 확인하세요.";
      throw new ApiError(0, "CONNECTION_ERROR", message + (options.method === "POST" ? " INSERT였다면 저장 여부를 확인한 뒤 재시도하세요." : ""));
    } finally { clearTimeout(timer); }
  }

  function setConnection(status, label) {
    const element = byId("connection-status");
    element.className = "connection-status " + (status === "ok" ? "is-connected" : status === "error" ? "is-error" : "");
    element.querySelector("span").textContent = label;
    state.connected = status === "ok";
    document.body.classList.toggle("is-offline", !state.connected);
  }

  function formatStorage(bytes) {
    if (bytes >= 1024 * 1024) return [(bytes / (1024 * 1024)).toFixed(2), "MiB"];
    return [(bytes / 1024).toFixed(2), "KiB"];
  }

  async function refreshOverview({ notify = false } = {}) {
    if (state.refreshing) { state.refreshPending ||= notify; return; }
    state.refreshing = true;
    byId("refresh-stats").classList.add("is-loading");
    try {
      const stats = await requestJSON("/stats");
      byId("stat-rows").textContent = numberFormat.format(stats.engine.row_count);
      byId("sidebar-row-count").textContent = numberFormat.format(stats.engine.row_count);
      byId("stat-height").textContent = numberFormat.format(stats.engine.index_height);
      const [size, unit] = formatStorage(stats.engine.file_bytes);
      byId("stat-storage").textContent = size;
      byId("storage-unit").textContent = unit;
      byId("stat-workers").textContent = numberFormat.format(stats.pool.workers);
      byId("stat-queue").textContent = `대기 ${stats.pool.queued} / ${stats.pool.queue_capacity} · 처리 중 ${stats.pool.active}`;
      setConnection("ok", "엔진 연결됨");
      if (notify) showToast("엔진 상태를 업데이트했습니다.");
    } catch (error) {
      setConnection("error", error.status === 401 ? "API 토큰 필요" : "연결 끊김");
      if (notify) showToast(error.status === 401 ? "연결 설정에서 API 토큰을 입력하세요." : error.message);
    } finally {
      state.refreshing = false;
      byId("refresh-stats").classList.remove("is-loading");
      if (state.refreshPending) {
        state.refreshPending = false;
        refreshOverview({ notify: true });
      }
    }
  }

  function activateView(view, focus = false) {
    document.querySelectorAll("[data-view]").forEach((tab) => {
      const active = tab.dataset.view === view;
      tab.setAttribute("aria-selected", String(active));
      tab.tabIndex = active ? 0 : -1;
      byId("view-" + tab.dataset.view).hidden = !active;
      if (active && focus) tab.focus();
    });
  }

  function showEmpty(title, description, showExample = false) {
    byId("results-empty").hidden = false;
    byId("results-empty").querySelector("h3").textContent = title;
    byId("results-empty").querySelector("p").textContent = description;
    byId("load-first-example").hidden = !showExample;
  }

  function hideResultContent() {
    ["results-table-wrap", "result-pagination", "insert-success", "results-empty", "result-metrics", "result-feedback"].forEach((id) => { byId(id).hidden = true; });
    byId("export-csv").disabled = true;
    byId("result-count").textContent = "0";
  }

  function renderTable() {
    const result = state.result;
    if (!result || !result.rows.length) return;
    byId("results-empty").hidden = true;
    byId("results-table-wrap").hidden = false;
    const header = node("tr");
    const rowNumberHeader = node("th", "row-number", "#");
    rowNumberHeader.scope = "col";
    header.append(rowNumberHeader);
    for (const column of result.columns) {
      const th = node("th", "", column);
      th.scope = "col";
      header.append(th);
    }
    byId("results-head").replaceChildren(header);
    const fragment = document.createDocumentFragment();
    const start = state.page * PAGE_SIZE;
    for (const [index, values] of result.rows.slice(start, start + PAGE_SIZE).entries()) {
      const row = node("tr");
      row.append(node("td", "row-number", start + index + 1));
      for (const value of values) {
        const td = node("td", typeof value === "number" ? "cell-number" : value === "" ? "cell-empty" : "", value === "" ? "빈 문자열" : value);
        td.title = String(value);
        row.append(td);
      }
      fragment.append(row);
    }
    byId("results-body").replaceChildren(fragment);
    byId("results-table-wrap").scrollTop = 0;
    byId("result-pagination").hidden = false;
    byId("pagination-label").textContent = `${numberFormat.format(start + 1)}–${numberFormat.format(Math.min(start + PAGE_SIZE, result.rows.length))} / ${numberFormat.format(result.rows.length)}행`;
    byId("previous-page").disabled = state.page === 0;
    byId("next-page").disabled = start + PAGE_SIZE >= result.rows.length;
    byId("export-csv").disabled = false;
  }

  function renderPlan(result) {
    const view = byId("view-plan");
    view.replaceChildren();
    const content = node("div", "plan-content");
    const header = node("div", "plan-header");
    header.append(node("span", "plan-badge" + (result.plan === "TABLE SCAN" ? " is-scan" : ""), result.plan),
      node("span", "language-label", result.explain ? "EXPLAIN" : "C ENGINE"));
    const descriptions = {
      "INDEX LOOKUP": "ID 인덱스에서 파일 위치를 찾고 해당 레코드를 읽습니다.",
      "INDEX RANGE SCAN": "B+ 트리에서 시작 키를 찾고 범위에 속한 레코드를 순서대로 조회합니다.",
      "TABLE SCAN": "조회 시작 시 공개된 레코드를 순회하며 WHERE 조건을 검사합니다.",
      "INSERT": "레코드를 파일에 저장하고 새 ID와 파일 위치를 B+ 트리에 등록합니다.",
    };
    const steps = result.plan === "INSERT" ? ["SQL 파서", "파일 저장", "인덱스 등록"] :
      result.plan === "TABLE SCAN" ? ["SQL 파서", "파일 순회", "조건 검사"] : ["SQL 파서", "B+ 트리", "파일 조회"];
    const flow = node("div", "plan-flow");
    steps.forEach((step, index) => {
      if (index) flow.append(node("span", "plan-arrow", "→"));
      flow.append(node("div", "plan-step", step));
    });
    const details = node("div", "plan-details");
    const examined = node("div", "plan-detail", "조건을 검사한 행");
    examined.append(node("strong", "", numberFormat.format(result.examined_rows)));
    const returned = node("div", "plan-detail", result.plan === "INSERT" ? "추가한 행" : "반환한 행");
    returned.append(node("strong", "", numberFormat.format(result.plan === "INSERT" ? result.affected_rows : result.row_count)));
    details.append(examined, returned);
    content.append(header, node("p", "plan-description", descriptions[result.plan] || "C 엔진이 반환한 실행 계획입니다."), flow, details,
      node("p", "plan-note", result.explain ? "계획만 확인했습니다. EXPLAIN은 데이터를 읽거나 변경하지 않습니다." : "실제 엔진 응답입니다. 검사한 행 수는 배치로 미리 읽은 물리적 행 수와 다를 수 있습니다."));
    view.append(content);
  }

  function renderSuccess(result, elapsed) {
    state.result = result;
    state.error = null;
    state.page = 0;
    hideResultContent();
    byId("result-count").textContent = numberFormat.format(result.row_count);
    byId("result-metrics").hidden = false;
    byId("metric-total").textContent = elapsed.toFixed(2) + " ms";
    byId("metric-engine").textContent = Number(result.request?.execution_ms || 0).toFixed(3) + " ms";
    byId("metric-worker").textContent = result.request?.worker || "";
    byId("json-response").textContent = JSON.stringify(result, null, 2);
    byId("query-timestamp").textContent = new Date().toLocaleTimeString("ko-KR", { hour12: false }) + " 실행";
    renderPlan(result);
    if (result.plan === "INSERT") {
      byId("insert-success").hidden = false;
      byId("insert-summary").textContent = `${numberFormat.format(result.affected_rows)}개의 레코드가 추가되었습니다`;
      byId("insert-detail").textContent = `첫 ID ${numberFormat.format(result.first_insert_id)} · 파일 저장과 인덱스 등록을 완료했습니다.`;
      activateView("table");
    } else if (result.explain) {
      showEmpty("실행 계획만 확인했습니다", "데이터를 읽지 않았습니다. 실행 계획 탭에서 검색 경로를 확인하세요.");
      activateView("plan");
    } else if (result.rows.length) {
      renderTable();
      activateView("table");
    } else {
      showEmpty("조회된 레코드가 없습니다", "조건에 맞는 행이 없거나 테이블이 비어 있습니다. 데이터를 추가하거나 WHERE 조건을 바꿔 보세요.");
      activateView("table");
    }
  }

  function renderFailure(error) {
    state.result = null;
    state.error = error;
    hideResultContent();
    const feedback = byId("result-feedback");
    feedback.replaceChildren(node("strong", "", `${error.status ? "HTTP " + error.status + " · " : ""}${error.code || "ERROR"}`), node("span", "", error.message));
    feedback.hidden = false;
    showEmpty("요청을 완료하지 못했습니다", error.status === 401 ? "상단의 연결 설정에서 API 토큰을 입력하세요." :
      !error.status || error.status >= 500 ? "INSERT 요청이었다면 저장 여부를 먼저 확인하세요. 오류 내용에 따라 엔진을 다시 열어야 할 수 있습니다." :
      "위의 오류 내용을 확인하고 쿼리를 수정해 주세요.");
    byId("view-plan").replaceChildren(node("div", "plan-empty", "성공한 실행 계획이 없습니다."));
    byId("json-response").textContent = JSON.stringify({ error: { code: error.code, message: error.message } }, null, 2);
    byId("query-timestamp").textContent = "마지막 요청 실패";
    if (error.status === 401) setConnection("error", "API 토큰 필요");
    else if (!error.status) setConnection("error", "연결 끊김");
    activateView("table");
  }

  async function runQuery(sql = selectedSQL()) {
    if (state.busy) return;
    if (!sql.trim()) { showToast("실행할 SQL을 입력하세요."); editor.focus(); return; }
    setBusy(true);
    hideResultContent();
    showEmpty("엔진에서 쿼리를 실행하고 있어요", "완료되면 결과와 실행 계획을 보여드립니다.");
    activateView("table");
    const start = performance.now();
    try {
      const result = await requestJSON("/query", { method: "POST", body: JSON.stringify({ sql }) });
      renderSuccess(result, performance.now() - start);
      rememberQuery(sql, true);
      setConnection("ok", "엔진 연결됨");
      await refreshOverview();
    } catch (error) {
      renderFailure(error);
      rememberQuery(sql, false);
    } finally { setBusy(false); }
  }

  function exportCSV() {
    const result = state.result;
    if (!result?.rows.length) return;
    const quote = (value) => '"' + String(value).replaceAll('"', '""') + '"';
    const lines = [result.columns, ...result.rows].map((row) => row.map(quote).join(",")).join("\r\n");
    const url = URL.createObjectURL(new Blob(["\ufeff", lines], { type: "text/csv;charset=utf-8" }));
    const link = node("a");
    link.href = url;
    link.download = "minidb-result-" + new Date().toISOString().slice(0, 10) + ".csv";
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    showToast("현재 조회 결과를 CSV로 내보냈습니다.");
  }

  function quoteSQL(value) { return "'" + value.replaceAll("'", "''") + "'"; }

  function bindEvents() {
    editor.maxLength = 65536;
    editor.addEventListener("input", updateEditorView);
    editor.addEventListener("scroll", syncEditorScroll);
    editor.addEventListener("keydown", (event) => {
      if (event.key === "Tab" && !event.shiftKey && !state.busy) {
        event.preventDefault();
        editor.setRangeText("  ", editor.selectionStart, editor.selectionEnd, "end");
        updateEditorView();
      }
    });
    document.addEventListener("keydown", (event) => {
      if ((event.ctrlKey || event.metaKey) && event.key === "Enter" && !document.querySelector("dialog[open]")) {
        event.preventDefault();
        runQuery();
      }
    });
    byId("run-query").addEventListener("click", () => runQuery());
    byId("explain-query").addEventListener("click", () => {
      const sql = selectedSQL();
      const keyword = firstKeyword(sql);
      if (!["SELECT", "EXPLAIN"].includes(keyword)) { showToast("SELECT의 실행 계획을 확인할 수 있습니다."); return; }
      const explained = keyword === "EXPLAIN" ? sql : "EXPLAIN " + sql;
      setEditor(explained);
      runQuery(explained);
    });
    document.querySelectorAll("[data-preset]").forEach((button) => {
      button.addEventListener("click", () => setEditor(presets[button.dataset.preset]));
    });
    byId("try-index").addEventListener("click", () => setEditor(presets.id));
    byId("load-first-example").addEventListener("click", () => { setEditor(presets.all); showToast("예제 쿼리를 불러왔습니다. 실행 버튼을 눌러 보세요."); });
    document.querySelectorAll("[data-view]").forEach((tab, index, tabs) => {
      tab.addEventListener("click", () => activateView(tab.dataset.view));
      tab.addEventListener("keydown", (event) => {
        if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
        event.preventDefault();
        const next = event.key === "Home" ? 0 : event.key === "End" ? tabs.length - 1 :
          (index + (event.key === "ArrowRight" ? 1 : -1) + tabs.length) % tabs.length;
        activateView(tabs[next].dataset.view, true);
      });
    });
    byId("previous-page").addEventListener("click", () => { if (state.page > 0) { state.page--; renderTable(); } });
    byId("next-page").addEventListener("click", () => { if (state.result && (state.page + 1) * PAGE_SIZE < state.result.rows.length) { state.page++; renderTable(); } });
    byId("export-csv").addEventListener("click", exportCSV);
    byId("refresh-stats").addEventListener("click", () => refreshOverview({ notify: true }));
    byId("clear-history").addEventListener("click", () => {
      state.history = [];
      try { localStorage.removeItem(HISTORY_KEY); } catch { /* Optional browser storage. */ }
      renderHistory();
      showToast("이 브라우저의 최근 쿼리를 지웠습니다.");
    });
    byId("open-connection").addEventListener("click", () => {
      byId("server-origin").value = location.origin;
      byId("api-token").value = state.token;
      byId("connection-dialog").showModal();
    });
    byId("connection-form").addEventListener("submit", (event) => {
      event.preventDefault();
      state.token = byId("api-token").value.trim();
      byId("api-token").value = "";
      byId("connection-dialog").close();
      refreshOverview({ notify: true });
    });
    document.querySelectorAll("[data-close-dialog]").forEach((button) => {
      button.addEventListener("click", () => button.closest("dialog").close());
    });
    byId("open-insert").addEventListener("click", () => {
      byId("insert-form-error").hidden = true;
      byId("insert-dialog").showModal();
    });
    byId("insert-form").addEventListener("submit", (event) => {
      event.preventDefault();
      if (state.busy) return;
      const name = byId("record-name").value;
      const email = byId("record-email").value;
      const ageText = byId("record-age").value.trim();
      const age = Number(ageText);
      let error = "";
      if (new TextEncoder().encode(name).length > 63) error = "이름은 UTF-8 기준 63바이트 이하여야 합니다.";
      else if (new TextEncoder().encode(email).length > 127) error = "이메일은 UTF-8 기준 127바이트 이하여야 합니다.";
      else if (!/^-?\d+$/.test(ageText) || !Number.isInteger(age) || age < -2147483648 || age > 2147483647) error = "나이는 INT32 범위의 정수로 입력하세요.";
      if (error) { byId("insert-form-error").textContent = error; byId("insert-form-error").hidden = false; return; }
      const sql = `INSERT INTO users (name, age, email)\nVALUES (${quoteSQL(name)}, ${age}, ${quoteSQL(email)});`;
      byId("insert-dialog").close();
      byId("insert-form").reset();
      setEditor(sql);
      runQuery(sql);
    });
    byId("insert-sample").addEventListener("click", () => { if (!state.busy) { setEditor(sampleSQL); runQuery(sampleSQL); } });
    byId("view-inserted").addEventListener("click", () => {
      if (!state.result || state.result.plan !== "INSERT" || state.busy) return;
      const first = state.result.first_insert_id;
      const last = first + state.result.affected_rows - 1;
      const sql = `SELECT *\nFROM users\nWHERE id BETWEEN ${first} AND ${last}\nLIMIT 100;`;
      setEditor(sql);
      runQuery(sql);
    });
    document.addEventListener("visibilitychange", () => {
      if (!document.hidden && !state.busy) refreshOverview();
    });
  }

  if (/Mac|iPhone|iPad/.test(navigator.platform)) {
    byId("shortcut-key").textContent = "⌘";
    document.querySelector(".shortcut-inline").textContent = "⌘ + Enter";
  }
  bindEvents();
  updateEditorView();
  renderHistory();
  refreshOverview();
  setInterval(() => { if (!document.hidden && !state.busy) refreshOverview(); }, 10000);
})();
