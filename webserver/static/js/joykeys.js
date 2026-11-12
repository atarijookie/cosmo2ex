/*================================================================
  Joystick keys of the remote pages (remote.html, remote_input.html)
  ================================================================
  When the IKBD settings have 'Enable Keys 0' / 'Enable Keys 1' on, ce_cores turns those keys into joystick 0 / 1 moves
  (ce_cores/ikbd/ikbd_usb.cpp processKeyboard()) - for the web UI keys the same way as for a USB keyboard. This file:
	- shows which keys are the joystick keys (#joykeys-info of the page), from GET /ikbd when the page opens:
		{ "keyboard_joy0": 1, "keyboard_keys_settings0": "A%S%D%W%LCTRL", ... }    keys: left % down % right % up % fire
	- sends those keys by position, down on press and up on release (POST /hid/keyboard), whatever key mode the page is
	  in - in the 'by character' mode the keys are sent as a quick down + up of the typed character, and the held
	  modifiers (e.g. LCTRL as fire) are released around it, which would make the joystick unusable
  Key names are the ones of the IKBD settings page (ikbd.html, ce_cores/ikbd/keytranslator.cpp), matched by key position
  (KeyboardEvent.code), like ce_cores matches the keys of a USB keyboard.

  The handlers run in the capture phase on window and are added before the other key handlers of the page (this file
  is loaded in <head>), so a joystick key never reaches them.
  ----------------------------------------------------------------*/

var CosmosEx = CosmosEx || {};
CosmosEx.JoyKeys = (function() {
	// KeyboardEvent.code -> key name of the IKBD settings, for the keys which are not a letter / digit (same as ikbd.html)
	var codeToName = {
		Escape: 'ESC', Backspace: 'BACKSPACE', Tab: 'TAB', Enter: 'ENTER', Space: 'SPACE',
		ControlLeft: 'LCTRL', ControlRight: 'RCTRL', ShiftLeft: 'LSHIFT', ShiftRight: 'RSHIFT', AltLeft: 'LALT', CapsLock: 'CAPS',
		F1: 'F1', F2: 'F2', F3: 'F3', F4: 'F4', F5: 'F5', F6: 'F6', F7: 'F7', F8: 'F8', F9: 'F9', F10: 'F10', F11: 'F11', F12: 'F12',
		Home: 'HOME', Insert: 'INSERT', Delete: 'DELETE', NumpadSubtract: 'KPMINUS', NumpadAdd: 'KPPLUS',
		ArrowUp: 'UP', ArrowDown: 'DOWN', ArrowLeft: 'LEFT', ArrowRight: 'RIGHT',
		NumpadDivide: 'KPSLASH', NumpadMultiply: 'KPASTERISK', NumpadDecimal: 'KPDOT', NumpadEnter: 'KPENTER',
		Numpad9: 'KPLPARENT', Numpad0: 'KPRPARENT'
	};

	// key name -> linux key code ce_cores looks for (KeyTranslator::humanKeyToPc())
	var nameToLinux = {
		ESC: 1, BACKSPACE: 14, TAB: 15, ENTER: 28, SPACE: 57, LCTRL: 29, RCTRL: 97, LSHIFT: 42, RSHIFT: 54, LALT: 56, CAPS: 58,
		F1: 59, F2: 60, F3: 61, F4: 62, F5: 63, F6: 64, F7: 65, F8: 66, F9: 67, F10: 68, F11: 87, F12: 88,
		HOME: 102, INSERT: 110, DELETE: 111, KPMINUS: 74, KPPLUS: 78, UP: 103, DOWN: 108, LEFT: 105, RIGHT: 106,
		KPSLASH: 98, KPASTERISK: 55, KPDOT: 83, KPENTER: 96, KPLPARENT: 179, KPRPARENT: 180,
		'1': 2, '2': 3, '3': 4, '4': 5, '5': 6, '6': 7, '7': 8, '8': 9, '9': 10, '0': 11,
		Q: 16, W: 17, E: 18, R: 19, T: 20, Y: 21, U: 22, I: 23, O: 24, P: 25,
		A: 30, S: 31, D: 32, F: 33, G: 34, H: 35, J: 36, K: 37, L: 38,
		Z: 44, X: 45, C: 46, V: 47, B: 48, N: 49, M: 50
	};

	var DIRS = ['left', 'down', 'right', 'up', 'fire'];	// order in the settings string

	var keyMap = {};			// key name of an enabled joystick -> linux code
	var held = {};				// KeyboardEvent.code -> linux code, joystick keys sent as down and not released yet
	var enabled = [null, null];	// enabled joystick -> its keys { left, down, right, up, fire }
	var infoEl = null;

	var queue = Promise.resolve();		// events go one after another - parallel requests could arrive out of order
	function sendKey(code, state) {
		queue = queue.then(function() {
			return fetch('/hid/keyboard', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ type: 'pc', code: code, state: state })
			}).catch(function() {});
		});
	}

	function nameOfCode(code) {
		if (!code) {
			return null;
		}
		var m = /^Key([A-Z])$/.exec(code) || /^Digit([0-9])$/.exec(code);
		if (m) {
			return m[1];
		}
		return codeToName[code] || null;
	}

	function parseKeys(str) {
		var parts = (str || '').split('%');
		if (parts.length !== 5) {
			return null;
		}
		var keys = {};
		for (var i = 0; i < 5; i++) {
			keys[DIRS[i]] = parts[i].trim().toUpperCase();
		}
		return keys;
	}

	// e.g. "WASD LCTRL" or "UP LEFT DOWN RIGHT RCTRL"
	function describeKeys(k) {
		var dirs = [k.up, k.left, k.down, k.right];
		var allSingle = dirs.every(function(d) { return d.length === 1; });
		return dirs.join(allSingle ? '' : ' ') + ' ' + k.fire;
	}

	function showInfo() {
		if (!infoEl) {
			return;
		}
		var lines = [];
		for (var j = 0; j < 2; j++) {
			if (enabled[j]) {
				lines.push('Joystick keys ' + j + ' are enabled: ' + describeKeys(enabled[j]));
			}
		}
		infoEl.textContent = lines.join('\n');
		infoEl.style.display = lines.length ? '' : 'none';
	}

	function releaseAll() {
		Object.keys(held).forEach(function(c) { sendKey(held[c], 'up'); });
		held = {};
	}

	function isOurControl(el) {		// typing into the selects / sliders of the page is not for the ST
		if (!el || !el.tagName) {
			return false;
		}
		var tag = el.tagName.toLowerCase();
		return tag === 'select' || (tag === 'input' && el.type === 'range') || !!(el.closest && el.closest('#charbar'));
	}

	function onKeyDown(e) {
		if (isOurControl(e.target)) {
			return;
		}
		var name = nameOfCode(e.code);
		var linux = name ? keyMap[name] : undefined;
		if (linux === undefined) {
			return;
		}
		e.preventDefault();
		e.stopImmediatePropagation();	// not to the key handlers of the page
		if (held[e.code] === undefined) {	// not on auto repeat
			held[e.code] = linux;
			sendKey(linux, 'down');
		}
	}

	function onKeyUp(e) {
		if (held[e.code] === undefined) {
			return;
		}
		e.preventDefault();
		e.stopImmediatePropagation();
		sendKey(held[e.code], 'up');
		delete held[e.code];
	}

	function apply(settings) {
		releaseAll();
		keyMap = {};
		enabled = [null, null];
		for (var j = 0; j < 2; j++) {
			if (settings['keyboard_joy' + j] !== 1) {
				continue;
			}
			var keys = parseKeys(settings['keyboard_keys_settings' + j]);
			if (!keys) {
				continue;
			}
			enabled[j] = keys;
			DIRS.forEach(function(d) {
				if (nameToLinux[keys[d]] !== undefined) {
					keyMap[keys[d]] = nameToLinux[keys[d]];
				}
			});
		}
		showInfo();
	}

	function load() {
		fetch('/ikbd', { method: 'GET' })
			.then(function(res) {
				if (!res.ok) {
					throw new Error('GET /ikbd failed: ' + res.status);
				}
				return res.json();
			})
			.then(apply)
			.catch(function(err) { console.log('joystick keys:', err); });
	}

	window.addEventListener('keydown', onKeyDown, true);
	window.addEventListener('keyup', onKeyUp, true);
	window.addEventListener('blur', releaseAll);		// the key ups go to another window, don't leave the joystick stuck
	load();

	// the 'Joystick keys 0 are enabled: ...' lines go to #joykeys-info of the page
	document.addEventListener('DOMContentLoaded', function() {
		infoEl = document.getElementById('joykeys-info');
		if (infoEl) {
			infoEl.style.whiteSpace = 'pre-line';
		}
		showInfo();
	});

	return {};
})();
