// Search worker: level-1 nested Monte Carlo search until the deadline.
function listMoves(b, R, C) {
  const W = C + 1;
  const S = new Int32Array((R + 1) * W), N = new Int32Array((R + 1) * W);
  for (let r = 0; r < R; r++) for (let c = 0; c < C; c++) {
    const v = b[r * C + c], i = (r + 1) * W + c + 1;
    S[i] = v + S[i - 1] + S[i - W] - S[i - W - 1];
    N[i] = (v ? 1 : 0) + N[i - 1] + N[i - W] - N[i - W - 1];
  }
  const sum = (A, r1, c1, r2, c2) => A[(r2 + 1) * W + c2 + 1] - A[r1 * W + c2 + 1] - A[(r2 + 1) * W + c1] + A[r1 * W + c1];
  const out = [];
  for (let r1 = 0; r1 < R; r1++) for (let r2 = r1; r2 < R; r2++) for (let c1 = 0; c1 < C; c1++) {
    if (sum(N, r1, c1, r2, c1) === 0) continue;
    for (let c2 = c1; c2 < C; c2++) {
      const s = sum(S, r1, c1, r2, c2);
      if (s > 10) break;
      if (s < 10) continue;
      if (sum(N, r1, c2, r2, c2) === 0) continue;
      if (sum(N, r1, c1, r1, c2) === 0 || sum(N, r2, c1, r2, c2) === 0) continue;
      out.push([r1, c1, r2, c2, sum(N, r1, c1, r2, c2)]);
    }
  }
  return out;
}
function applyMove(b, C, m) {
  const nb = b.slice();
  for (let r = m[0]; r <= m[2]; r++) for (let c = m[1]; c <= m[3]; c++) nb[r * C + c] = 0;
  return nb;
}
// rollout policy: among legal rectangles, take one that removes the fewest apples
function pick(ms) {
  let mn = 99; for (const m of ms) if (m[4] < mn) mn = m[4];
  const c = ms.filter(m => m[4] === mn);
  return c[(Math.random() * c.length) | 0];
}
function rollout(b, R, C) {
  let s = 0; const seq = [];
  for (;;) { const ms = listMoves(b, R, C); if (!ms.length) return { s, seq }; const m = pick(ms); b = applyMove(b, C, m); s += m[4]; seq.push(m); }
}
// level-1 nested Monte Carlo search; on deadline, finishes with the best line found so far
function nmcs(board, R, C, deadline) {
  let b = board, score = 0, bestSeq = [], bestTotal = -1; const done = [];
  for (;;) {
    const ms = listMoves(b, R, C); if (!ms.length) break;
    for (const m of ms) {
      const r = rollout(applyMove(b, C, m), R, C), tot = score + m[4] + r.s;
      if (tot > bestTotal) { bestTotal = tot; bestSeq = [m, ...r.seq]; }
      if (Date.now() > deadline) return { score: bestTotal, moves: done.concat(bestSeq) };
    }
    const m = bestSeq.shift(); done.push(m); b = applyMove(b, C, m); score += m[4];
  }
  return { score, moves: done };
}
onmessage = e => {
  const { board, R, C, deadline } = e.data;
  const b = Int8Array.from(board);
  let best = { score: -1, moves: [] }, runs = 0;
  do { const r = nmcs(b, R, C, deadline); runs++; if (r.score > best.score) best = r; } while (Date.now() < deadline);
  postMessage({ score: best.score, moves: best.moves, runs });
};
