/* SPDX-License-Identifier: GPL-3.0-or-later */
'use strict';
const $ = id => document.getElementById(id);
let socket = null, terminal = null, fit = null, paired = false, selected = false, connecting = false;
const consoleMode = new URLSearchParams(location.search).has('console');
if (consoleMode) document.body.classList.add('console-mode');
let controllerMode = 'terminal';
const encoder = new TextEncoder();
const keyBytes = {esc:'\x1b',tab:'\t',up:'\x1b[A',down:'\x1b[B',left:'\x1b[D',right:'\x1b[C',interrupt:'\x03',enter:'\r'};
function notice(message) { $('notice').textContent = message; $('notice').hidden = !message; }
async function request(path, body) {
  const response = await fetch(path, body === undefined ? {} : {method:'POST',headers:{'X-PS5-Client':'1'},body});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `Connection failed (${response.status})`);
  return data;
}
function send(data) {
  if (socket?.readyState !== WebSocket.OPEN) return;
  const bytes = encoder.encode(data);
  for (let i = 0; i < bytes.length; i += 8192) socket.send(bytes.subarray(i, i + 8192));
}
function resize() {
  if (!terminal || $('terminal-view').hidden) return;
  fit.fit(); $('geometry').textContent = `${terminal.cols} × ${terminal.rows}`;
  if (socket?.readyState === WebSocket.OPEN) socket.send(`resize:${terminal.cols}:${terminal.rows}`);
}
async function connect() {
  if (!paired) { $('pair-dialog').showModal(); return; }
  if (connecting || (socket && socket.readyState <= WebSocket.OPEN)) return;
  connecting = true;
  selected = true; $('picker').hidden = true; $('terminal-view').hidden = false;
  const fontSize = consoleMode ? 18 : 14;
  try {
    await Promise.all([document.fonts.load(`${fontSize}px "PS5 Terminal Mono"`), document.fonts.load(`bold ${fontSize}px "PS5 Terminal Mono"`)]);
  } catch (_) { notice('Terminal font could not load. Reopen the app to retry.'); }
  if (!terminal) {
    terminal = new Terminal({cursorBlink:true,convertEol:false,scrollback:3000,fontSize,fontFamily:'"PS5 Terminal Mono", monospace',letterSpacing:0,lineHeight:1.1,theme:{background:'#111315',foreground:'#e6e8e7',cursor:'#acf1c6'},allowProposedApi:false});
    fit = new FitAddon.FitAddon(); terminal.loadAddon(fit); terminal.open($('terminal'));
    terminal.onData(send);
    new ResizeObserver(resize).observe($('terminal'));
  }
  resize(); notice(''); $('connection').textContent = 'Opening Codex'; $('reconnect').hidden = true;
  const current = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/terminal/codex?cols=${terminal.cols}&rows=${terminal.rows}`);
  current.binaryType = 'arraybuffer'; socket = current; connecting = false; controllerMode = 'terminal';
  current.onopen = () => { if (socket !== current) return; $('connection').textContent = 'Codex connected'; $('disconnect').hidden = false; resize(); terminal.focus(); };
  current.onmessage = event => {
    if (socket !== current) return;
    if (event.data instanceof ArrayBuffer) terminal.write(new Uint8Array(event.data));
    else notice(event.data);
  };
  current.onclose = () => {
    if (socket !== current) return;
    socket = null; $('connection').textContent = 'Disconnected'; $('disconnect').hidden = true; $('reconnect').hidden = false;
    notice('Terminal disconnected. Reconnect to the running CLI. If the payload exited, start it again on PS5.');
  };
  current.onerror = () => notice('Could not open Codex. Check that the payload is running and another device is not controlling the terminal.');
}
$('launch-codex').onclick = connect;
$('reconnect').onclick = async () => {
  try { paired = (await request('/api/status')).paired; connect(); } catch (error) { notice(error.message); }
};
$('disconnect').onclick = () => socket?.close(1000);
$('pair-form').onsubmit = async event => {
  event.preventDefault(); $('pair-error').textContent = '';
  try { await request('/api/pair', $('pair-code').value); paired = true; $('pair-code').value = ''; $('pair-dialog').close(); $('connection').textContent = 'PS5 connected'; if (selected) connect(); }
  catch (error) { $('pair-error').textContent = error.message; }
};
$('pair-dialog').addEventListener('cancel', event => event.preventDefault());
for (const button of document.querySelectorAll('[data-key]')) button.onclick = () => { send(keyBytes[button.dataset.key]); terminal?.focus(); };
$('keyboard-button').onclick = () => { $('keyboard-dialog').showModal(); $('keyboard-text').focus(); };
$('keyboard-close').onclick = () => { $('keyboard-dialog').close(); terminal?.focus(); };
$('keyboard-send').onclick = () => { send($('keyboard-text').value); $('keyboard-text').value = ''; };
$('keyboard-enter').onclick = () => { if ($('keyboard-text').value) $('keyboard-send').click(); send('\r'); };
for (const label of '1234567890qwertyuiopasdfghjkl;zxcvbnm,./'.split('').concat(['Space','⌫'])) {
  const button = document.createElement('button'); button.textContent = label;
  button.onclick = () => { const input = $('keyboard-text'); input.value = label === '⌫' ? input.value.slice(0,-1) : input.value + (label === 'Space' ? ' ' : label); };
  $('keyboard-grid').append(button);
}
// A terminal consumes controller keys directly. Options moves focus to its
// toolbar; a dialog temporarily uses normal button navigation.
let previousButtons = new Set(), repeatAt = new Map(), lastControllerAction = null;
function moveControllerFocus(direction, scope) {
  const controls = Array.from(scope.querySelectorAll('button:not(:disabled), input, textarea')).filter(el => el.getClientRects().length && !el.closest('.xterm'));
  if (!controls.length) return;
  const active = controls.includes(document.activeElement) ? document.activeElement : null;
  if (!active) { controls[0].focus(); return; }
  const rect = active.getBoundingClientRect(), x = rect.x + rect.width / 2, y = rect.y + rect.height / 2;
  let best = null, score = Infinity;
  for (const control of controls) {
    if (control === active) continue;
    const box = control.getBoundingClientRect(), dx = box.x + box.width / 2 - x, dy = box.y + box.height / 2 - y;
    const forward = direction === 12 ? -dy : direction === 13 ? dy : direction === 14 ? -dx : dx;
    const sideways = direction < 14 ? Math.abs(dx) : Math.abs(dy);
    if (forward > 4 && forward + sideways * 3 < score) { best = control; score = forward + sideways * 3; }
  }
  best?.focus();
}
function controllerAction(button, source, now = performance.now()) {
  document.body.classList.add('controller-input');
  // Some console browsers emit both keyboard and Gamepad events for one press.
  if (lastControllerAction?.button === button && lastControllerAction.source !== source && now - lastControllerAction.at < 120) return;
  lastControllerAction = {button,source,at:now};
  const dialog = document.querySelector('dialog[open]');
  const inTerminal = terminal && !$('terminal-view').hidden;
  if (dialog) {
    if (button === 1 && dialog.id === 'keyboard-dialog') $('keyboard-close').click();
    else if (button === 0) {
      if (document.activeElement?.tagName === 'BUTTON') document.activeElement.click();
      else if (dialog.id === 'pair-dialog') $('pair-form').requestSubmit();
      else $('keyboard-grid').querySelector('button')?.focus();
    } else if (button >= 12 && button <= 15) moveControllerFocus(button, dialog);
    return;
  }
  if (inTerminal && button === 9) {
    controllerMode = controllerMode === 'terminal' ? 'controls' : 'terminal';
    if (controllerMode === 'controls') $('keyboard-button').focus(); else terminal.focus();
    return;
  }
  if (inTerminal && controllerMode === 'terminal') {
    const keys = {0:'enter',1:'esc',3:'tab',12:'up',13:'down',14:'left',15:'right'};
    if (keys[button]) { send(keyBytes[keys[button]]); terminal.focus(); }
    else if (button === 2) { $('keyboard-button').click(); $('keyboard-grid').querySelector('button')?.focus(); }
    return;
  }
  if (button === 0) {
    if (document.activeElement?.tagName === 'BUTTON') document.activeElement.click();
    else if (!inTerminal) $('launch-codex').focus();
  } else if (button === 1 && inTerminal) { controllerMode = 'terminal'; terminal.focus(); }
  else if (button >= 12 && button <= 15) moveControllerFocus(button, document);
}
function gamepadFrame(now) {
  const pad = Array.from(navigator.getGamepads?.() || []).find(pad => pad && pad.connected !== false);
  const pressed = new Set();
  if (pad && !document.hidden) {
    pad.buttons.forEach((button,index) => { if (button.pressed) pressed.add(index); });
    if (pad.axes[0] < -.6) pressed.add(14); else if (pad.axes[0] > .6) pressed.add(15);
    if (pad.axes[1] < -.6) pressed.add(12); else if (pad.axes[1] > .6) pressed.add(13);
    for (const button of pressed) {
      const direction = button >= 12 && button <= 15;
      if (!previousButtons.has(button)) { controllerAction(button, 'pad', now); repeatAt.set(button, now + 400); }
      else if (direction && now >= repeatAt.get(button)) { controllerAction(button, 'pad', now); repeatAt.set(button, now + 120); }
    }
  }
  for (const button of previousButtons) if (!pressed.has(button)) repeatAt.delete(button);
  previousButtons = pressed;
  requestAnimationFrame(gamepadFrame);
}
requestAnimationFrame(gamepadFrame);
window.addEventListener('pointerdown', () => document.body.classList.remove('controller-input'));
window.addEventListener('keydown', event => {
  document.body.classList.add('controller-input');
  if (!consoleMode || event.isComposing || event.keyCode === 229 || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
  const keys = {Enter:0,Escape:1,BrowserBack:1,ArrowUp:12,ArrowDown:13,ArrowLeft:14,ArrowRight:15};
  const button = keys[event.key];
  if (button === undefined || (document.activeElement === $('keyboard-text') && button !== 1) || document.activeElement === $('pair-code')) return;
  event.preventDefault(); event.stopImmediatePropagation();
  if (!event.repeat || button >= 12) controllerAction(button, 'key');
}, true);
(async () => {
  try {
    const status = await request('/api/status'); paired = status.paired; $('fixture').hidden = !status.fixture;
    if (!paired && consoleMode) {
      try { paired = (await request('/api/pair-local', '')).paired; }
      catch (_) { /* Remote clients still pair using the notification code. */ }
    }
    $('connection').textContent = paired ? 'PS5 connected' : 'Pair your device';
    if (!paired) $('pair-dialog').showModal(); else $('launch-codex').focus();
  } catch (error) { $('connection').textContent = 'Unavailable'; notice(error.message); }
})();
