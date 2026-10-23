const $ = id => document.getElementById(id);
const state = { R: 10, C: 17, board: [], low: new Set(), moves: [], steps: [], score: 0, step: 0, sel: -1, solving: false,
  live: null, track: null, base: 0 }; // base: apples already removed before a re-solve
const NW = Math.max(1, Math.min(8, navigator.hardwareConcurrency || 4));
// the game tab this window works on (set from the icon click that opened it)
const q = new URLSearchParams(location.search);
const target = { tabId: +q.get("tab") || null, windowId: +q.get("win") || null };

// ---------- capture & recognition ----------
// Chrome allows about two captures per second; wait and retry when over that limit.
async function captureTab(windowId, opts) {
  for (let t = 0; ; t++) {
    try { return await chrome.tabs.captureVisibleTab(windowId, opts); }
    catch (e) { if (t >= 5 || !/MAX_CAPTURE/.test(e.message)) throw e; await new Promise(r => setTimeout(r, 600)); }
  }
}
async function captureAndSolve() {
  setStatus("캡처 중…");
  try {
    const tab = target.tabId && await chrome.tabs.get(target.tabId).catch(() => null);
    if (!tab) return setStatus("게임 탭을 찾지 못했습니다. 게임 탭에서 확장프로그램 아이콘을 다시 눌러 주세요.", true);
    if (!tab.active) return setStatus("게임 탭이 그 창에서 선택된 상태여야 캡처할 수 있습니다.", true);
    chrome.scripting.executeScript({ target: { tabId: tab.id, allFrames: true }, files: ["content.js"] }).catch(() => {});
    stopTracking(); state.base = 0;
    const url = await captureTab(tab.windowId, { format: "png" });
    const boxes = await handleImage(await (await fetch(url)).blob());
    if (boxes) startTracking(tab, boxes);
    return boxes;
  } catch (e) {
    setStatus("캡처하지 못했습니다: " + e.message, true);
  }
}

async function handleImage(blob) {
  setStatus("인식 중…");
  const bmp = await createImageBitmap(blob);
  const cv = new OffscreenCanvas(bmp.width, bmp.height);
  const ctx = cv.getContext("2d", { willReadFrequently: true }); ctx.drawImage(bmp, 0, 0);
  const g = findApples(ctx.getImageData(0, 0, cv.width, cv.height).data, cv.width, cv.height);
  if (!g) return setStatus("사과를 찾지 못했습니다. 게임 판 전체가 화면에 보이게 해 주세요.", true);
  const R = g.rows.length, C = g.cols.length, board = new Array(R * C).fill(0), low = new Set();
  const boxes = { w: cv.width, h: cv.height, rows: g.rows, cols: g.cols, cells: new Array(R * C).fill(null) };
  for (const a of g.apples) {
    const r = classify(a.f), i = a.row * C + a.col;
    board[i] = r.digit;
    if (r.margin < 1.2) low.add(i);
    boxes.cells[i] = [a.x0, a.y0, a.x1, a.y1];
  }
  await loadBoard(board, R, C, low, `인식 ${g.apples.length}개 (${R}×${C})`);
  return boxes;
}

// ---------- live tracking ----------
// Re-captures the game tab and marks an apple gone once its spot has no red in two reads in a row.
// Chrome allows about two captures per second, so this polls every 600 ms.
function startTracking(tab, boxes) {
  state.live = new Uint8Array(state.R * state.C);
  const miss = new Uint8Array(state.R * state.C);
  const tick = async () => {
    if (!state.track || state.track.busy) return;
    state.track.busy = true;
    try {
      const [cur] = await chrome.tabs.query({ active: true, windowId: tab.windowId });
      if (!cur || cur.id !== tab.id) return setLive("게임 탭이 앞에 있을 때만 판을 따라갑니다.");
      const r = await readBoard(tab, boxes, i => state.live[i]);
      if (r.error) return setLive(r.error);
      const { absent, present } = r;
      // a single drag removes few apples; a mass disappearance means the board is hidden or scrolled
      if (absent.length > 20 && absent.length > present) return setLive("판이 보이지 않습니다. 스크롤이나 창을 처음 캡처한 상태로 돌려 주세요.");
      setLive("");
      let changed = false;
      for (let i = 0; i < miss.length; i++) miss[i] = absent.includes(i) ? miss[i] + 1 : 0;
      for (const i of absent) if (miss[i] >= 2 && !state.live[i]) { state.live[i] = 1; changed = true; }
      if (changed) { autoStep(); render(); }
    } catch (e) {
      setLive("판을 따라가지 못했습니다: " + e.message);
    } finally {
      if (state.track) state.track.busy = false;
    }
  };
  state.track = { timer: setInterval(tick, 600), busy: false };
}
// captures the tab and lists the apple spots (from the first capture) that no longer show red
async function readBoard(tab, boxes, skip = () => false) {
  const url = await captureTab(tab.windowId, { format: "jpeg", quality: 70 });
  const bmp = await createImageBitmap(await (await fetch(url)).blob());
  if (bmp.width !== boxes.w || bmp.height !== boxes.h) return { error: "창 크기가 바뀌었습니다. 「캡처 & 풀기」를 다시 눌러 주세요." };
  const cv = new OffscreenCanvas(bmp.width, bmp.height), ctx = cv.getContext("2d", { willReadFrequently: true });
  ctx.drawImage(bmp, 0, 0);
  const absent = [];
  let present = 0;
  boxes.cells.forEach((b, i) => {
    if (!b || skip(i)) return;
    const d = ctx.getImageData(b[0], b[1], b[2] - b[0] + 1, b[3] - b[1] + 1).data;
    let red = 0, n = 0;
    for (let p = 0; p < d.length; p += 12) { n++; if (d[p] > 150 && d[p] - d[p + 1] > 90 && d[p] - d[p + 2] > 90) red++; }
    if (red / n < 0.15) absent.push(i); else present++;
  });
  return { absent, present };
}
function stopTracking() {
  if (state.track) clearInterval(state.track.timer);
  state.track = null; state.live = null; setLive("");
}
function setLive(t) { $("live").textContent = t; $("live").hidden = !t; }

const moveDone = m => {
  for (let r = m[0]; r <= m[2]; r++) for (let c = m[1]; c <= m[3]; c++) {
    const i = r * state.C + c;
    if (state.board[i] && !state.live[i]) return false;
  }
  return true;
};
// with live tracking the current step is the first one that still has an apple left
function autoStep() {
  const k = state.steps.findIndex(s => s.some(i => !moveDone(state.moves[i])));
  state.step = k < 0 ? state.steps.length : k;
}

// ---------- solving ----------
// Solid rectangles (no empty cell inside) need no earlier move, so they go first in
// top-left order (row, then column); moves that span emptied cells keep their order after them.
function orderMoves(moves) {
  const solid = m => (m[2] - m[0] + 1) * (m[3] - m[1] + 1) === m[4];
  const first = moves.filter(solid).sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  return first.concat(moves.filter(m => !solid(m)));
}

function solve(prefix = "", budget = +$("budget").value) {
  const { R, C, board } = state;
  const deadline = Date.now() + budget, t0 = performance.now();
  state.solving = true; state.moves = []; state.steps = []; state.step = 0; render();
  setStatus(`탐색 중… (${budget / 1000}초)`);
  $("capture").disabled = $("resolve").disabled = true;
  let left = NW, best = { score: -1, moves: [] }, runs = 0, done;
  const finished = new Promise(res => { done = res; });
  for (let k = 0; k < NW; k++) {
    const w = new Worker("worker.js");
    w.onmessage = e => {
      runs += e.data.runs;
      if (e.data.score > best.score) best = e.data;
      w.terminate();
      if (--left === 0) {
        state.solving = false; state.moves = orderMoves(best.moves); state.steps = buildSteps(state.moves); state.score = best.score; state.step = 0;
        if (state.live) autoStep();
        $("capture").disabled = $("resolve").disabled = false;
        setStatus(`${prefix}${((performance.now() - t0) / 1000).toFixed(1)}초 탐색 · ${best.moves.length}수`);
        render();
        done();
      }
    };
    w.onerror = e => setStatus("탐색 오류: " + e.message, true);
    w.postMessage({ board, R, C, deadline });
  }
  return finished;
}

// ---------- auto drag ----------
// Drags through chrome.debugger (Input.dispatchMouseEvent), which the page sees as real mouse input.
// Attaching shows Chrome's "debugging" bar and can shift the page, so the board is captured after attaching.
const auto = { running: false, stop: false };
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function autoRun() {
  if (auto.running) { auto.stop = true; return; }
  const tab = target.tabId && await chrome.tabs.get(target.tabId).catch(() => null);
  if (!tab) return setStatus("게임 탭을 찾지 못했습니다. 게임 탭에서 확장프로그램 아이콘을 다시 눌러 주세요.", true);
  const dbg = { tabId: tab.id };
  Object.assign(auto, { running: true, stop: false });
  $("auto").textContent = "자동 실행 중지";
  try {
    await chrome.debugger.attach(dbg, "1.3");
    const view = await waitStable(dbg);
    const boxes = await captureAndSolve();
    if (!boxes) throw new Error("판을 인식하지 못했습니다.");
    if (state.R !== 10 || state.C !== 17)
      throw new Error(`판이 ${state.R}행 × ${state.C}열만 보입니다. 디버깅 안내 띠 때문에 화면이 줄었을 수 있으니, 창을 키우거나 스크롤해서 판 전체가 보이게 한 뒤 다시 눌러 주세요.`);
    if (!state.moves.length) throw new Error("풀이를 만들지 못했습니다.");
    // captured pixels -> CSS pixels. devicePixelRatio (includes page zoom) is not thrown off by a
    // scrollbar the way innerWidth is; fall back to the width ratio if the capture was scaled.
    const kDpr = 1 / view.dpr, kW = view.w / boxes.w;
    const k = Math.abs(boxes.w * kDpr - view.w) <= 40 ? kDpr : kW;
    // Drag the whole plan, then look at the real board: apples a drag missed are still there, so
    // re-solve from what is left (1 s search) and drag again until no rectangle is left or nothing changes.
    let total = 0, round = 1;
    for (;;) {
      total += await dragAll(dbg, boxes, k, round);
      if (auto.stop) break;
      await sleep(1000); // removal animation
      const r = await readBoard(tab, boxes, i => !state.board[i]);
      if (r.error) throw new Error(r.error);
      if (!r.absent.length) throw new Error("드래그가 게임에 반영되지 않았습니다. 게임 화면이 처음 캡처한 위치 그대로인지 확인해 주세요.");
      for (const i of r.absent) { state.board[i] = 0; state.base++; }
      if (state.live) state.live.fill(0);
      buildBoard();
      await solve(`${++round}번째 바퀴 · `, 1000);
      if (!state.moves.length) break;
    }
    setStatus(auto.stop ? `자동 실행을 멈췄습니다 (${total}수 드래그).` : `자동 실행 완료: ${state.base}점 · ${round}바퀴 · ${total}수 드래그.`);
  } catch (e) {
    setStatus("자동 실행 오류: " + e.message, true);
  } finally {
    chrome.debugger.detach(dbg).catch(() => {});
    auto.running = false;
    $("auto").textContent = "자동 실행";
  }
}
// waits until the viewport stops changing (the debugging bar slides in after attaching)
async function waitStable(dbg) {
  const read = async () => (await chrome.debugger.sendCommand(dbg, "Runtime.evaluate",
    { expression: "[innerWidth, innerHeight, devicePixelRatio]", returnByValue: true })).result.value;
  let last = await read(), same = 0;
  for (let t = 0; t < 25 && same < 4; t++) {
    await sleep(200);
    const v = await read();
    same = v.join() === last.join() ? same + 1 : 0;
    last = v;
  }
  return { w: last[0], h: last[1], dpr: last[2] };
}
async function dragAll(dbg, boxes, k, round) {
  const { rows, cols } = boxes;
  const px = cols.length > 1 ? (cols[cols.length - 1] - cols[0]) / (cols.length - 1) : 40;
  const py = rows.length > 1 ? (rows[rows.length - 1] - rows[0]) / (rows.length - 1) : 40;
  const order = state.steps.slice(state.step).flat();
  let n = 0;
  for (const i of order) {
    if (auto.stop) break;
    const m = state.moves[i];
    // from just outside the top-left apple to just outside the bottom-right one, staying clear of neighbours
    await drag(dbg, (cols[m[1]] - px * 0.45) * k, (rows[m[0]] - py * 0.45) * k, (cols[m[3]] + px * 0.45) * k, (rows[m[2]] + py * 0.45) * k);
    setStatus(`${round}번째 바퀴 · 자동 드래그 ${++n} / ${order.length}`);
    await sleep(+$("gap").value);
  }
  return n;
}
async function drag(dbg, x1, y1, x2, y2) {
  const send = (type, x, y, extra) => chrome.debugger.sendCommand(dbg, "Input.dispatchMouseEvent", { type, x, y, ...extra });
  await send("mouseMoved", x1, y1, { button: "none" });
  await send("mousePressed", x1, y1, { button: "left", buttons: 1, clickCount: 1 });
  for (let s = 1; s <= 4; s++) await send("mouseMoved", x1 + (x2 - x1) * s / 4, y1 + (y2 - y1) * s / 4, { button: "left", buttons: 1 });
  await send("mouseReleased", x2, y2, { button: "left", buttons: 0, clickCount: 1 });
}
chrome.debugger.onDetach.addListener(() => { if (auto.running) auto.stop = true; });

// ---------- UI ----------
function setStatus(t, err) { $("status").textContent = t; $("status").classList.toggle("err", !!err); }

function loadBoard(board, R, C, low, msg) {
  Object.assign(state, { board, R, C, low, sel: -1 });
  const warn = [], n = board.filter(v => v).length;
  if (R !== 10 || C !== 17) warn.push(`격자가 ${R}×${C}로 인식되었습니다 (보통 10×17).`);
  if (n !== R * C) warn.push(`빈 칸 ${R * C - n}개가 있습니다.`);
  if (low.size) warn.push(`확신도 낮은 칸 ${low.size}개 (주황 테두리). 실제 숫자와 비교해 주세요.`);
  $("warn").hidden = !warn.length; $("warn").textContent = warn.join(" ");
  buildBoard();
  return solve(msg + " · ");
}

function buildBoard() {
  const { R, C } = state, el = $("board");
  el.style.gridTemplateColumns = `repeat(${C}, minmax(0, 1fr))`;
  el.innerHTML = "";
  for (let i = 0; i < R * C; i++) {
    const b = document.createElement("button");
    b.type = "button"; b.className = "cell"; b.dataset.i = i;
    b.style.gridRow = ((i / C) | 0) + 1; b.style.gridColumn = (i % C) + 1;
    b.setAttribute("aria-label", `${((i / C) | 0) + 1}행 ${(i % C) + 1}열`);
    el.appendChild(b);
  }
  render();
}

// One step shows every solid rectangle at once (they are independent); each later move is its own step.
function buildSteps(moves) {
  const solid = m => (m[2] - m[0] + 1) * (m[3] - m[1] + 1) === m[4];
  let n = 0; while (n < moves.length && solid(moves[n])) n++;
  const steps = n ? [Array.from({ length: n }, (_, i) => i)] : [];
  for (let i = n; i < moves.length; i++) steps.push([i]);
  return steps;
}

function render() {
  const { R, C, board, moves, steps, step, low, sel } = state;
  const gone = new Uint8Array(R * C), stepOf = [];
  steps.forEach((s, k) => s.forEach(i => { stepOf[i] = k; }));
  let removed = 0;
  if (state.live) {
    state.live.forEach((g, i) => { if (g && board[i]) { gone[i] = 1; removed++; } });
  } else moves.forEach((m, i) => {
    if (stepOf[i] >= step) return;
    removed += m[4];
    for (let r = m[0]; r <= m[2]; r++) for (let c = m[1]; c <= m[3]; c++) gone[r * C + c] = 1;
  });
  // an apple gone that the plan only removes in a later step means the board left the plan
  let off = false;
  if (state.live) {
    const owner = new Int32Array(R * C).fill(-1);
    moves.forEach((m, i) => {
      for (let r = m[0]; r <= m[2]; r++) for (let c = m[1]; c <= m[3]; c++) if (board[r * C + c] && owner[r * C + c] < 0) owner[r * C + c] = i;
    });
    gone.forEach((g, i) => { if (g && (owner[i] < 0 || stepOf[owner[i]] > step)) off = true; });
  }
  $("off").hidden = !off;
  const el = $("board");
  el.querySelectorAll(".cell").forEach(b => {
    const i = +b.dataset.i, v = board[i];
    b.textContent = v || "";
    b.className = "cell" + (!v ? " empty" : gone[i] ? " gone" : "") + (low.has(i) ? " low" : "") + (sel === i ? " sel" : "");
  });
  el.querySelectorAll(".rect").forEach(d => d.remove());
  const place = (cls, m) => {
    const d = document.createElement("div"); d.className = "rect " + cls;
    d.style.gridArea = `${m[0] + 1} / ${m[1] + 1} / ${m[2] + 2} / ${m[3] + 2}`;
    el.appendChild(d);
  };
  (steps[step] || []).forEach(i => { if (!state.live || !moveDone(moves[i])) place("now", moves[i]); });
  (steps[step + 1] || []).forEach(i => place("next", moves[i]));
  const total = board.filter(v => v).length;
  const base = state.base, all = base + total;
  $("score").innerHTML = state.solving ? `… <small>/ ${all}</small>` : board.length ? `${base + state.score} <small>/ ${all}</small>` : "– <small>/ –</small>";
  $("stepinfo").textContent = steps.length ? `${Math.min(step + 1, steps.length)} / ${steps.length} 단계 · 현재 ${base + removed}점` : (state.solving ? "탐색 중…" : "–");
  const cur = (steps[step] || []).map(i => moves[i]).filter(m => !state.live || !moveDone(m));
  $("cur").textContent = cur.length > 1 ? `꽉 찬 사각형 ${cur.length}개 (순서 상관없음) · 사과 ${cur.reduce((a, m) => a + m[4], 0)}개`
    : cur.length ? `${cur[0][0] + 1}~${cur[0][2] + 1}행, ${cur[0][1] + 1}~${cur[0][3] + 1}열 · 사과 ${cur[0][4]}개`
    : (moves.length ? "끝. 더 지울 수 있는 사각형이 없습니다." : "–");
  $("prev").disabled = step === 0; $("next").disabled = step >= steps.length;
  const ol = $("moves");
  if (ol.childElementCount !== moves.length) {
    ol.innerHTML = moves.map((m, k) => `<li data-k="${k}"><span>${k + 1}.</span><span>${m[0] + 1}-${m[2] + 1}행 ${m[1] + 1}-${m[3] + 1}열</span><span>+${m[4]}</span></li>`).join("");
  }
  ol.querySelectorAll("li").forEach((li, k) => { li.classList.toggle("done", stepOf[k] < step); li.classList.toggle("on", stepOf[k] === step); });
  const on = ol.children[(steps[step] || [])[0]]; if (on) on.scrollIntoView({ block: "nearest" });
}

function go(d) { const s = Math.max(0, Math.min(state.steps.length, state.step + d)); if (s !== state.step) { state.step = s; render(); } }
$("capture").onclick = captureAndSolve;
$("auto").onclick = autoRun;
chrome.runtime.onMessage.addListener(msg => {
  if (msg.cmd === "next") go(1);
  else if (msg.cmd === "prev") go(-1);
  else if (msg.cmd === "target") { target.tabId = msg.tabId; target.windowId = msg.windowId; setStatus("게임 탭을 다시 지정했습니다. 「캡처 & 풀기」를 누르세요."); }
});
$("next").onclick = () => go(1);
$("prev").onclick = () => go(-1);
$("resolve").onclick = () => {
  if (state.solving || !state.board.length) return;
  if (state.live) { state.live.forEach((g, i) => { if (g && state.board[i]) { state.board[i] = 0; state.base++; } }); state.live.fill(0); buildBoard(); }
  solve("현재 판 · ");
};
$("moves").onclick = e => { const li = e.target.closest("li"); if (li) { state.step = state.steps.findIndex(s => s.includes(+li.dataset.k)); render(); } };
$("board").onclick = e => {
  const b = e.target.closest(".cell"); if (!b) return;
  state.sel = state.sel === +b.dataset.i ? -1 : +b.dataset.i; render();
};
document.addEventListener("keydown", e => {
  if (e.target.tagName === "SELECT" || e.ctrlKey || e.metaKey || e.altKey) return;
  if (state.sel >= 0 && /^[0-9]$/.test(e.key)) {
    state.board[state.sel] = +e.key; state.low.delete(state.sel); state.sel = -1;
    e.preventDefault(); loadBoard(state.board, state.R, state.C, state.low, "칸 수정");
    return;
  }
  if (e.key === "Escape") { state.sel = -1; render(); return; }
  if (e.key === " " || e.key === "ArrowRight" || e.key === "Enter") { e.preventDefault(); go(1); }
  else if (e.key === "ArrowLeft" || e.key === "Backspace") { e.preventDefault(); go(-1); }
});
render();
