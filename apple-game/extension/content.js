// Runs on the game page: forwards Space/→ (next) and ← (previous) to the side panel,
// so the steps can be advanced right after dragging without clicking the panel.
window.addEventListener("keydown", e => {
  if (e.ctrlKey || e.metaKey || e.altKey || /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName)) return;
  const cmd = e.key === " " || e.key === "ArrowRight" ? "next" : e.key === "ArrowLeft" ? "prev" : null;
  if (!cmd) return;
  e.preventDefault();
  chrome.runtime.sendMessage({ cmd }).catch(() => {});
}, true);
