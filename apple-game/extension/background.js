// Clicking the toolbar icon opens the solver in its own window, aimed at the tab the icon was clicked on.
async function openPanel(tab) {
  const { panelWin } = await chrome.storage.session.get("panelWin");
  if (panelWin) {
    try {
      await chrome.windows.update(panelWin, { focused: true });
      chrome.runtime.sendMessage({ cmd: "target", tabId: tab.id, windowId: tab.windowId }).catch(() => {});
      return;
    } catch { /* window was closed */ }
  }
  const game = await chrome.windows.get(tab.windowId);
  const opts = { url: `panel.html?tab=${tab.id}&win=${tab.windowId}`, type: "popup", width: 420, height: 920 };
  // beside the game window if that fits on screen; otherwise Chrome picks the position
  const win = await chrome.windows.create({ ...opts, left: Math.max(0, (game.left ?? 0) + (game.width ?? 0)), top: Math.max(0, game.top ?? 0) })
    .catch(() => chrome.windows.create(opts));
  await chrome.storage.session.set({ panelWin: win.id });
}
chrome.action.onClicked.addListener(openPanel);
chrome.windows.onRemoved.addListener(async id => {
  const { panelWin } = await chrome.storage.session.get("panelWin");
  if (panelWin === id) chrome.storage.session.remove("panelWin");
});
