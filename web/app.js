/* SPDX-License-Identifier: GPL-3.0-or-later */
'use strict';
const $ = id => document.getElementById(id);
let socket = null, terminal = null, fit = null, paired = false, selected = false, connecting = false, restarting = false;
const consoleMode = new URLSearchParams(location.search).has('console');
if (consoleMode) document.body.classList.add('console-mode');
let controllerMode = 'terminal';
let controllerFocus = null, activatingControl = false;
const encoder = new TextEncoder();
const keyBytes = {esc:'\x1b',tab:'\t',up:'\x1b[A',down:'\x1b[B',left:'\x1b[D',right:'\x1b[C',interrupt:'\x03',enter:'\r'};
function notice(message) { $('notice').textContent = message; $('notice').hidden = !message; }
function focusPicker() {
  $('launch-codex').focus();
  if (consoleMode) controllerFocus = $('launch-codex');
}
async function request(path, body) {
  const response = await fetch(path, body === undefined ? {} : {method:'POST',headers:{'X-PS5-Client':'1'},body});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `Connection failed (${response.status})`);
  return data;
}
function send(data) {
  if (socket?.readyState !== WebSocket.OPEN) {
    notice('Reconnect to Codex before typing.');
    return false;
  }
  terminal?.scrollToBottom();
  const bytes = encoder.encode(data);
  for (let i = 0; i < bytes.length; i += 8192) socket.send(bytes.subarray(i, i + 8192));
  return true;
}
function setInputConnected(connected) {
  if (terminal) terminal.options.disableStdin = !connected;
  for (const button of document.querySelectorAll('[data-key], #focus-prompt, #keyboard-send, #keyboard-enter')) button.disabled = !connected;
}
function focusPrompt() {
  terminal?.scrollToBottom();
  terminal?.focus();
}
function resize() {
  if (!terminal || $('terminal-view').hidden) return;
  const columns = terminal.cols, rows = terminal.rows;
  fit.fit(); $('geometry').textContent = `${terminal.cols} × ${terminal.rows}`;
  if (socket?.readyState === WebSocket.OPEN && (columns !== terminal.cols || rows !== terminal.rows)) socket.send(`resize:${terminal.cols}:${terminal.rows}`);
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
  setInputConnected(false);
  resize(); notice(''); $('connection').textContent = 'Opening Codex'; $('reconnect').hidden = true;
  const current = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/terminal/codex?cols=${terminal.cols}&rows=${terminal.rows}`);
  current.binaryType = 'arraybuffer'; socket = current; connecting = false; controllerMode = 'terminal';
  current.onopen = () => { if (socket !== current) return; $('connection').textContent = 'Codex connected'; $('disconnect').hidden = false; setInputConnected(true); resize(); focusPrompt(); };
  current.onmessage = event => {
    if (socket !== current) return;
    if (event.data instanceof ArrayBuffer) terminal.write(new Uint8Array(event.data));
    else notice(event.data);
  };
  current.onclose = () => {
    if (socket !== current) return;
    socket = null; $('disconnect').hidden = true; setInputConnected(false);
    if (restarting) return;
    $('connection').textContent = 'Disconnected'; $('reconnect').hidden = false;
    notice('Terminal disconnected. Reconnect to the running CLI, or use Restart CLI if it stopped responding.');
  };
  current.onerror = () => { if (!restarting) notice('Could not open Codex. If it exited, use Restart CLI. Another device may already control the terminal.'); };
}
$('launch-codex').onclick = connect;
$('reconnect').onclick = async () => {
  try { paired = (await request('/api/status')).paired; connect(); } catch (error) { notice(error.message); }
};
$('disconnect').onclick = () => socket?.close(1000);
$('focus-prompt').onclick = focusPrompt;
$('restart-cli').onclick = async () => {
  if (restarting) return;
  restarting = true;
  setInputConnected(false);
  $('restart-cli').disabled = true;
  $('restart-cli').textContent = 'Restarting…';
  $('reconnect').hidden = true;
  $('connection').textContent = 'Restarting Codex';
  notice('Restarting Codex. Saved sign-in and files are kept.');
  try {
    await request('/api/cli/codex/restart', '');
    const previous = socket; socket = null;
    previous?.close();
    terminal?.reset();
    connecting = false;
    await connect();
  } catch (error) {
    $('connection').textContent = 'Restart failed';
    $('reconnect').hidden = false;
    notice(error.message);
  } finally {
    restarting = false;
    $('restart-cli').disabled = false;
    $('restart-cli').textContent = 'Restart CLI';
  }
};
$('pair-form').onsubmit = async event => {
  event.preventDefault(); $('pair-error').textContent = '';
  try { await request('/api/pair', $('pair-code').value); paired = true; $('pair-code').value = ''; $('pair-dialog').close(); $('connection').textContent = 'PS5 connected'; if (selected) connect(); else focusPicker(); }
  catch (error) { $('pair-error').textContent = error.message; }
};
$('pair-dialog').addEventListener('cancel', event => event.preventDefault());
let deviceTimer = null, deviceRequest = 0;
async function showDeviceCode() {
  const current = ++deviceRequest;
  $('device-code').textContent = '…';
  $('device-expiry').textContent = 'Getting a code…';
  $('device-renew').disabled = true;
  clearInterval(deviceTimer);
  try {
    const data = await request('/api/pairing-code', '');
    if (current !== deviceRequest || !$('device-dialog').open) return;
    const expires = Date.now() + data.expires_in * 1000;
    $('device-code').textContent = `${data.code.slice(0, 3)} ${data.code.slice(3)}`;
    const update = () => {
      const seconds = Math.max(0, Math.ceil((expires - Date.now()) / 1000));
      $('device-expiry').textContent = seconds ? `Valid for ${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}` : 'Code expired. Select New code to try again.';
      if (!seconds) { $('device-code').textContent = 'Expired'; clearInterval(deviceTimer); }
    };
    update(); deviceTimer = setInterval(update, 1000);
  } catch (error) { if (current === deviceRequest) { $('device-code').textContent = '—'; $('device-expiry').textContent = error.message; } }
  finally { if (current === deviceRequest) $('device-renew').disabled = false; }
}
$('pair-device').onclick = () => {
  $('device-dialog').showModal();
  controllerFocus = $('device-close'); controllerFocus.focus();
  showDeviceCode();
};
$('device-renew').onclick = showDeviceCode;
$('device-close').onclick = () => $('device-dialog').close();
$('device-dialog').addEventListener('close', () => {
  deviceRequest++;
  clearInterval(deviceTimer); $('device-code').textContent = '';
  controllerFocus = $('pair-device'); controllerFocus.focus();
});
for (const button of document.querySelectorAll('[data-key]')) button.onclick = () => { send(keyBytes[button.dataset.key]); terminal?.focus(); };
$('keyboard-button').onclick = () => {
  $('keyboard-dialog').showModal();
  controllerFocus = consoleMode ? $('keyboard-grid').querySelector('button') : null;
  (controllerFocus || $('keyboard-text')).focus();
};
$('keyboard-close').onclick = () => { controllerFocus = null; $('keyboard-dialog').close(); terminal?.focus(); };
$('keyboard-send').onclick = () => {
  if (!$('keyboard-text').value) return;
  if (socket?.readyState !== WebSocket.OPEN) { notice('Reconnect to Codex before typing.'); return; }
  // Bracketed paste keeps a long or multiline message together in the CLI.
  terminal.paste($('keyboard-text').value);
  $('keyboard-text').value = '';
};
$('keyboard-enter').onclick = () => { if ($('keyboard-text').value) $('keyboard-send').click(); send('\r'); };
function typeKeyboard(text, backspace = false) {
  const input = $('keyboard-text');
  if (backspace && !input.value) { send('\x7f'); return; }
  let start = input.selectionStart, end = input.selectionEnd;
  if (backspace && start === end) start = Array.from(input.value.slice(0, start)).slice(0, -1).join('').length;
  input.setRangeText(text, start, end, 'end');
}
let keyboardShift = false, keyboardSymbols = false;
const letterKeys = '1234567890qwertyuiopasdfghjkl;zxcvbnm,./';
const symbolKeys = '!@#$%^&*()-_+=[]{}\\|:;"\'`~<>?/1234567890';
const characterButtons = Array.from(letterKeys, (_, index) => {
  const button = document.createElement('button');
  button.onclick = () => typeKeyboard(button.textContent);
  button.dataset.character = String(index);
  $('keyboard-grid').append(button);
  return button;
});
function renderKeyboard() {
  const labels = keyboardSymbols ? symbolKeys : keyboardShift ? letterKeys.toUpperCase() : letterKeys;
  characterButtons.forEach((button, index) => { button.textContent = labels[index]; });
  $('keyboard-shift').setAttribute('aria-pressed', String(keyboardShift));
  $('keyboard-symbols').textContent = keyboardSymbols ? 'ABC' : '#+=';
}
for (const [id, label, action] of [
  ['keyboard-shift','Shift',() => { keyboardShift = !keyboardShift; renderKeyboard(); }],
  ['keyboard-symbols','#+=',() => { keyboardSymbols = !keyboardSymbols; renderKeyboard(); }],
  ['keyboard-space','Space',() => typeKeyboard(' ')],
  ['keyboard-backspace','Backspace',() => typeKeyboard('', true)]
]) {
  const button = document.createElement('button'); button.id = id; button.textContent = label; button.onclick = action;
  if (id === 'keyboard-backspace') button.setAttribute('aria-label', 'Backspace');
  if (id === 'keyboard-symbols') button.setAttribute('aria-label', 'Switch letters and symbols');
  $('keyboard-grid').append(button);
}
renderKeyboard();

function activateControl(button) {
  activatingControl = true;
  try { button.click(); } finally { activatingControl = false; }
}
// A terminal consumes controller keys directly. Options moves focus to its
// toolbar; a dialog temporarily uses normal button navigation.
let previousButtons = new Set(), repeatAt = new Map(), lastControllerAction = null;
function moveControllerFocus(direction, scope) {
  const controls = Array.from(scope.querySelectorAll('button:not(:disabled), input, textarea')).filter(el => el.getClientRects().length && !el.closest('.xterm'));
  if (!controls.length) return;
  const active = controls.includes(document.activeElement) ? document.activeElement : null;
  if (!active) { controllerFocus = controls[0]; controllerFocus.focus(); return; }
  const rect = active.getBoundingClientRect(), x = rect.x + rect.width / 2, y = rect.y + rect.height / 2;
  let best = null, score = Infinity;
  for (const control of controls) {
    if (control === active) continue;
    const box = control.getBoundingClientRect(), dx = box.x + box.width / 2 - x, dy = box.y + box.height / 2 - y;
    const forward = direction === 12 ? -dy : direction === 13 ? dy : direction === 14 ? -dx : dx;
    const sideways = direction < 14 ? Math.abs(dx) : Math.abs(dy);
    if (forward > 4 && forward + sideways * 3 < score) { best = control; score = forward + sideways * 3; }
  }
  if (best) { controllerFocus = best; best.focus(); }
}
function controllerAction(button, source, now = performance.now()) {
  document.body.classList.add('controller-input');
  // Some console browsers emit both keyboard and Gamepad events for one press.
  if (lastControllerAction?.button === button && lastControllerAction.source !== source && now - lastControllerAction.at < 120) return;
  lastControllerAction = {button,source,at:now};
  const dialog = document.querySelector('dialog[open]');
  const inTerminal = terminal && !$('terminal-view').hidden;
  if (dialog) {
    if (button === 1 && dialog.id === 'device-dialog') activateControl($('device-close'));
    else if (button === 1 && dialog.id === 'keyboard-dialog') activateControl($('keyboard-close'));
    else if (button === 0) {
      const focused = controllerFocus && dialog.contains(controllerFocus) ? controllerFocus : document.activeElement;
      if (focused?.tagName === 'BUTTON') activateControl(focused);
      else if (dialog.id === 'pair-dialog') $('pair-form').requestSubmit();
      else { controllerFocus = $('keyboard-grid').querySelector('button'); controllerFocus?.focus(); }
    } else if (button >= 12 && button <= 15) moveControllerFocus(button, dialog);
    return;
  }
  if (inTerminal && button === 9) {
    controllerMode = controllerMode === 'terminal' ? 'controls' : 'terminal';
    controllerFocus = controllerMode === 'controls' ? $('keyboard-button') : null;
    if (controllerFocus) controllerFocus.focus(); else terminal.focus();
    return;
  }
  if (inTerminal && controllerMode === 'terminal') {
    const keys = {0:'enter',1:'esc',3:'tab',12:'up',13:'down',14:'left',15:'right'};
    if (keys[button]) { controllerFocus = null; send(keyBytes[keys[button]]); terminal.focus(); }
    else if (button === 2) {
      activateControl($('keyboard-button'));
      controllerFocus = $('keyboard-grid').querySelector('button'); controllerFocus?.focus();
    }
    return;
  }
  if (button === 0) {
    const focused = controllerFocus || document.activeElement;
    if (focused?.tagName === 'BUTTON') activateControl(focused);
    else if (!inTerminal) $('launch-codex').focus();
  } else if (button === 1 && inTerminal) { controllerFocus = null; controllerMode = 'terminal'; terminal.focus(); }
  else if (button >= 12 && button <= 15) moveControllerFocus(button, document);
}
function gamepadFrame(now) {
  let pad = null;
  try { pad = Array.from(navigator.getGamepads?.() || []).find(pad => pad && pad.connected !== false); }
  catch (_) { /* Console click/arrow input remains available without Gamepad access. */ }
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
// PS5's webview reports Cross as a mouse click, including clicks on the
// terminal's hidden textarea or the page background. Preserve D-pad focus
// through that click's pointerdown; actual pointer movement restores pointing.
let pointerPosition = null;
function usePointer() { controllerFocus = null; document.body.classList.remove('controller-input'); }
window.addEventListener('pointermove', event => {
  const moved = Math.abs(event.movementX || 0) + Math.abs(event.movementY || 0) > 2 ||
    (pointerPosition && Math.abs(event.clientX - pointerPosition.x) + Math.abs(event.clientY - pointerPosition.y) > 2);
  if (moved) usePointer();
  pointerPosition = {x:event.clientX,y:event.clientY};
});
window.addEventListener('pointerdown', event => { if (event.pointerType === 'touch') usePointer(); });
window.addEventListener('click', event => {
  if (!consoleMode || activatingControl || event.button > 0 || !(event.target instanceof Element)) return;
  const dialog = document.querySelector('dialog[open]');
  const focused = controllerFocus && controllerFocus.getClientRects().length ? controllerFocus : null;
  const navigateControl = focused && (dialog ? dialog.contains(focused) : !$('picker').hidden || controllerMode === 'controls');
  const terminalClick = !dialog && terminal && !$('terminal-view').hidden && controllerMode === 'terminal' &&
    (event.target.closest('#terminal') || event.target === document.body || event.target === document.documentElement);
  if (!navigateControl && !terminalClick) return;
  if (terminalClick && (terminal.hasSelection() || window.getSelection()?.toString())) return;
  event.preventDefault(); event.stopImmediatePropagation();
  if (navigateControl) focused.focus();
  controllerAction(0, 'click');
}, true);
window.addEventListener('keydown', event => {
  document.body.classList.add('controller-input');
  if (event.key === 'Tab') controllerFocus = null;
  if (!$('picker').hidden && !document.querySelector('dialog[open]') && ['ArrowUp','ArrowDown'].includes(event.key)) {
    const choices = Array.from(document.querySelectorAll('#pair-device:not([hidden]), .cli-option:not(:disabled)'));
    const selectedIndex = choices.indexOf(document.activeElement);
    const step = event.key === 'ArrowDown' ? 1 : -1;
    const next = choices[(selectedIndex + step + choices.length) % choices.length];
    if (next) { event.preventDefault(); event.stopImmediatePropagation(); controllerFocus = next; next.focus(); }
    return;
  }
  if ($('keyboard-dialog').open && event.target !== $('keyboard-text') && event.key.length === 1 &&
      !event.isComposing && !event.altKey && !event.ctrlKey && !event.metaKey) {
    event.preventDefault(); controllerFocus = null; typeKeyboard(event.key); $('keyboard-text').focus(); return;
  }
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
    $('pair-device').hidden = !(paired && consoleMode);
    if (!paired) $('pair-dialog').showModal(); else focusPicker();
  } catch (error) { $('connection').textContent = 'Unavailable'; notice(error.message); }
})();
