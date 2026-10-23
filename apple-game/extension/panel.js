const $ = id => document.getElementById(id);
const state = { R: 10, C: 17, board: [], low: new Set(), moves: [], steps: [], score: 0, step: 0, sel: -1, solving: false };
const NW = Math.max(1, Math.min(8, navigator.hardwareConcurrency || 4));

// ---------- capture & recognition ----------
async function captureAndSolve() {
  setStatus("캡처 중…");
  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    if (!tab) return setStatus("현재 탭을 찾지 못했습니다.", true);
    const url = await chrome.tabs.captureVisibleTab(tab.windowId, { format: "png" });
    await handleImage(await (await fetch(url)).blob());
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
  for (const a of g.apples) {
    const r = classify(a.f), i = a.row * C + a.col;
    board[i] = r.digit;
    if (r.margin < 1.2) low.add(i);
  }
  loadBoard(board, R, C, low, `인식 ${g.apples.length}개 (${R}×${C})`);
}

// ---------- solving ----------
// Solid rectangles (no empty cell inside) need no earlier move, so they go first in
// top-left order (row, then column); moves that span emptied cells keep their order after them.
function orderMoves(moves) {
  const solid = m => (m[2] - m[0] + 1) * (m[3] - m[1] + 1) === m[4];
  const first = moves.filter(solid).sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  return first.concat(moves.filter(m => !solid(m)));
}

function solve(prefix = "") {
  const { R, C, board } = state;
  const budget = +$("budget").value, deadline = Date.now() + budget, t0 = performance.now();
  state.solving = true; state.moves = []; state.steps = []; state.step = 0; render();
  setStatus(`탐색 중… (${budget / 1000}초)`);
  $("capture").disabled = $("resolve").disabled = true;
  let left = NW, best = { score: -1, moves: [] }, runs = 0;
  for (let k = 0; k < NW; k++) {
    const w = new Worker("worker.js");
    w.onmessage = e => {
      runs += e.data.runs;
      if (e.data.score > best.score) best = e.data;
      w.terminate();
      if (--left === 0) {
        state.solving = false; state.moves = orderMoves(best.moves); state.steps = buildSteps(state.moves); state.score = best.score; state.step = 0;
        $("capture").disabled = $("resolve").disabled = false;
        setStatus(`${prefix}${((performance.now() - t0) / 1000).toFixed(1)}초 탐색 · ${best.moves.length}수`);
        render();
      }
    };
    w.onerror = e => setStatus("탐색 오류: " + e.message, true);
    w.postMessage({ board, R, C, deadline });
  }
}

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
  solve(msg + " · ");
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
  moves.forEach((m, i) => {
    if (stepOf[i] >= step) return;
    removed += m[4];
    for (let r = m[0]; r <= m[2]; r++) for (let c = m[1]; c <= m[3]; c++) gone[r * C + c] = 1;
  });
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
  (steps[step] || []).forEach(i => place("now", moves[i]));
  (steps[step + 1] || []).forEach(i => place("next", moves[i]));
  const total = board.filter(v => v).length;
  $("score").innerHTML = state.solving ? `… <small>/ ${total}</small>` : board.length ? `${state.score} <small>/ ${total}</small>` : "– <small>/ –</small>";
  $("stepinfo").textContent = steps.length ? `${Math.min(step + 1, steps.length)} / ${steps.length} 단계 · 현재 ${removed}점` : (state.solving ? "탐색 중…" : "–");
  const cur = (steps[step] || []).map(i => moves[i]);
  $("cur").textContent = cur.length > 1 ? `꽉 찬 사각형 ${cur.length}개 한꺼번에 · 사과 ${cur.reduce((a, m) => a + m[4], 0)}개`
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
chrome.runtime.onMessage.addListener(msg => { if (msg.cmd === "next") go(1); else if (msg.cmd === "prev") go(-1); });
$("next").onclick = () => go(1);
$("prev").onclick = () => go(-1);
$("resolve").onclick = () => { if (!state.solving && state.board.length) solve(); };
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
