/*================================================================
  Gamepads of the browser as ST joysticks
  ================================================================
  /hid/joystick 	POST
	{ "type": "axis", "joy": 0, "axis": 0, "value": -32767 }    axis 0 X, 1 Y; - = left / up, like Linux joystick API
	{ "type": "button", "joy": 0, "button": 0, "state": "down" }

  ce_cores needs only to know the direction and if any button is held, so X, Y and one fire button are sent, and only when
  they change. A browser shows a gamepad only after a button on it was pressed - the first one shown is joystick 0, the
  second one joystick 1 (the IKBD setting 'First Joy is 0' decides which ST port that is).
  ----------------------------------------------------------------*/

var CosmosEx = CosmosEx || {};
CosmosEx.Gamepads = function() {
	var AXIS_MAX = 32767;
	var AXIS_THRESHOLD = 0.5;			// stick moved this far counts as direction (ce_cores takes 16000 of 32767 as direction)
	var POLL_MS = 16;
	// D-pad buttons in the 'standard' gamepad mapping
	var DPAD_UP = 12, DPAD_DOWN = 13, DPAD_LEFT = 14, DPAD_RIGHT = 15;

	var slots = [null, null];			// joystick 0 / 1 -> { index, id, x, y, fire }, x / y / fire as sent to ST, null = not sent yet
	var statusEl = null;

	var queue = Promise.resolve();		// events go one after another - parallel requests could arrive out of order
	function post(data) {
		queue = queue.then(function() {
			return fetch('/hid/joystick', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify(data)
			}).catch(function() {});
		});
	}

	function sendState(joy, x, y, fire) {
		var slot = slots[joy];
		if (x !== slot.x) {
			slot.x = x;
			post({ type: 'axis', joy: joy, axis: 0, value: x * AXIS_MAX });
		}
		if (y !== slot.y) {
			slot.y = y;
			post({ type: 'axis', joy: joy, axis: 1, value: y * AXIS_MAX });
		}
		if (fire !== slot.fire) {
			slot.fire = fire;
			post({ type: 'button', joy: joy, button: 0, state: fire ? 'down' : 'up' });
		}
	}

	function axisDir(pad, axis) {		// -1, 0, 1
		var value = pad.axes[axis] || 0;
		return value < -AXIS_THRESHOLD ? -1 : (value > AXIS_THRESHOLD ? 1 : 0);
	}

	function isPressed(pad, button) {
		return !!pad.buttons[button] && pad.buttons[button].pressed;
	}

	function sendPadState(joy, pad) {
		var x = axisDir(pad, 0);
		var y = axisDir(pad, 1);
		var standard = pad.mapping === 'standard';

		if (standard) {					// D-pad wins over the stick
			if (isPressed(pad, DPAD_LEFT)) { x = -1; }
			if (isPressed(pad, DPAD_RIGHT)) { x = 1; }
			if (isPressed(pad, DPAD_UP)) { y = -1; }
			if (isPressed(pad, DPAD_DOWN)) { y = 1; }
		}

		var fire = false;
		for (var i = 0; i < pad.buttons.length; i++) {
			var isDpad = standard && i >= DPAD_UP && i <= DPAD_RIGHT;
			if (!isDpad && isPressed(pad, i)) {
				fire = true;
			}
		}

		sendState(joy, x, y, fire);
	}

	function slotOfPad(pad) {
		for (var i = 0; i < slots.length; i++) {
			if (slots[i] && slots[i].index === pad.index && slots[i].id === pad.id) {
				return i;
			}
		}
		return -1;
	}

	function isPadThere(pads, slot) {
		var pad = pads[slot.index];
		return !!pad && pad.connected && pad.id === slot.id;
	}

	function showStatus() {
		if (!statusEl) {
			return;
		}
		var names = [];
		for (var i = 0; i < slots.length; i++) {
			if (slots[i]) {
				names.push('Joystick ' + i + ': ' + slots[i].id);
			}
		}
		statusEl.textContent = names.length ? names.join(' · ') : 'Gamepad: none - press a button on it to activate it';
	}

	function poll() {
		var pads = navigator.getGamepads();
		var focused = document.hasFocus();
		var i, changed = false;

		for (i = 0; i < slots.length; i++) {			// gamepad gone - nothing may stay held on the ST
			if (slots[i] && !isPadThere(pads, slots[i])) {
				sendState(i, 0, 0, false);
				slots[i] = null;
				changed = true;
			}
		}

		for (i = 0; i < pads.length; i++) {				// new gamepads take the free joysticks
			var pad = pads[i];
			if (pad && pad.connected && slotOfPad(pad) < 0) {
				var free = slots.indexOf(null);
				if (free >= 0) {
					slots[free] = { index: pad.index, id: pad.id, x: null, y: null, fire: null };
					changed = true;
				}
			}
		}

		for (i = 0; i < slots.length; i++) {
			if (slots[i]) {
				if (focused) {
					sendPadState(i, pads[slots[i].index]);
				} else {								// browser may freeze the gamepad data - like for keys, don't leave it stuck on the ST
					sendState(i, 0, 0, false);
				}
			}
		}

		if (changed) {
			showStatus();
		}
	}

	return {
		init: function(element) {
			statusEl = element;
			if (!navigator.getGamepads) {
				if (statusEl) {
					statusEl.textContent = 'Gamepad: not supported by this browser';
				}
				return;
			}
			showStatus();
			window.setInterval(poll, POLL_MS);
		}
	};
}();
