/*================================================================
  On-screen joysticks of the remote pages (remote.html, remote_input.html)
  ================================================================
  'Show joysticks' / 'Hide joysticks' button (#showjoysticks) shows two joysticks at the bottom right of the window,
  right of the ST keyboard and above the special characters bar.
	- clicking / touching them sends joystick events (POST /hid/joystick, like a gamepad): D-pad cell = direction
	  (diagonals too, sliding to another cell changes it), FIRE = fire; several fingers at once work
	- they light up what the ST gets from all sources: these clicks, gamepads (gamepad.js) and the joystick keys
	  (joykeys.js) - those call CosmosEx.JoyPanel.show(source, joy, x, y, fire)
  Joystick 0 / 1 here is the same number as in gamepad.js and the IKBD 'Keys 0' / 'Keys 1' settings (the IKBD setting
  'First Joy is 0' decides which ST port that is).

  Mouse / touch on the panel and on the button don't reach the remote control handlers (script.js), so they don't click
  or move the ST mouse.
  ----------------------------------------------------------------*/

var CosmosEx = CosmosEx || {};
CosmosEx.JoyPanel = (function() {
	var AXIS_MAX = 32767;
	var SOURCES = ['mouse', 'keys', 'pad'];

	// [joy][source] -> { x, y, fire } - x / y: -1, 0, 1
	var state = [{}, {}];
	var panel = null;
	var button = null;
	var shown = false;

	// pointers (mouse / fingers) held on the panel: pointerId -> { joy, fire } or { joy, x, y }
	var pointers = {};
	var sent = [{ x: 0, y: 0, fire: false }, { x: 0, y: 0, fire: false }];	// what the panel clicks sent to ce_cores

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

	var css = [
		'#joypanel { position: fixed; right: 0; bottom: var(--charbar-height, 44px); z-index: 1450; display: none;',
		'  padding: 8px 10px; background: rgba(40, 40, 40, 0.95); border-radius: 8px 0 0 0;',
		'  box-shadow: 0 -2px 10px rgba(0, 0, 0, 0.3); user-select: none; -webkit-user-select: none; touch-action: none; }',
		'body.joypanel-shown #joypanel { display: block; }',
		'body.joypanel-shown #keyboardlayer { padding-right: 230px; box-sizing: border-box; }',		// keyboard centered left of the panel
		'#joypanel .joy { display: flex; align-items: center; gap: 12px; }',
		'#joypanel .joy + .joy { margin-top: 8px; }',
		'#joypanel .joy-label { color: #ccc; font-size: 11px; margin-bottom: 2px; }',
		'#joypanel .joy-dpad { display: grid; grid-template-columns: repeat(3, 34px); grid-template-rows: repeat(3, 34px); gap: 2px; }',
		'#joypanel .joy-cell { display: flex; align-items: center; justify-content: center; font-size: 14px; color: #222;',
		'  background: #f4f4f4; border: 1px solid #999; border-radius: 4px; cursor: pointer; box-sizing: border-box; }',
		'#joypanel .joy-cell.diag { font-size: 10px; color: #777; background: #ddd; }',
		'#joypanel .joy-cell.center { background: #555; border-color: #555; cursor: default; }',
		'#joypanel .joy-fire { width: 60px; height: 60px; border-radius: 50%; display: flex; align-items: center; justify-content: center;',
		'  font-size: 12px; font-weight: bold; color: #fff; background: #b22; border: 2px solid #811; cursor: pointer; box-sizing: border-box; }',
		'#joypanel .joy-cell.on, #joypanel .joy-fire.on { background: #1e6fd9; border-color: #1858ad; color: #fff; }'
	].join('\n');

	// D-pad cells: [x, y, text]
	var CELLS = [
		[-1, -1, '◤'], [0, -1, '▲'], [1, -1, '◥'],
		[-1,  0, '◀'], [0,  0, ''],       [1,  0, '▶'],
		[-1,  1, '◣'], [0,  1, '▼'], [1,  1, '◢']
	];

	function build() {
		var style = document.createElement('style');
		style.textContent = css;
		document.head.appendChild(style);

		panel = document.createElement('div');
		panel.id = 'joypanel';
		for (var j = 0; j < 2; j++) {
			var label = document.createElement('div');
			label.className = 'joy-label';
			label.textContent = 'Joystick ' + j;
			panel.appendChild(label);

			var row = document.createElement('div');
			row.className = 'joy';
			var dpad = document.createElement('div');
			dpad.className = 'joy-dpad';
			CELLS.forEach(function(c) {
				var cell = document.createElement('div');
				var center = (c[0] === 0 && c[1] === 0);
				cell.className = 'joy-cell' + (center ? ' center' : '') + ((c[0] && c[1]) ? ' diag' : '');
				cell.textContent = c[2];
				if (!center) {
					cell.setAttribute('data-joy', j);
					cell.setAttribute('data-x', c[0]);
					cell.setAttribute('data-y', c[1]);
				}
				dpad.appendChild(cell);
			});
			row.appendChild(dpad);

			var fire = document.createElement('div');
			fire.className = 'joy-fire';
			fire.textContent = 'FIRE';
			fire.setAttribute('data-joy', j);
			fire.setAttribute('data-fire', '1');
			row.appendChild(fire);
			panel.appendChild(row);
		}
		document.body.appendChild(panel);

		// nothing from the panel to the remote control handlers (ST mouse, double tap, context menu)
		['mousedown', 'mouseup', 'mousemove', 'click', 'dblclick', 'contextmenu', 'touchstart', 'touchmove', 'touchend', 'touchcancel', 'wheel']
			.forEach(function(name) {
				panel.addEventListener(name, function(e) {
					e.stopPropagation();
					if (name !== 'wheel') {
						e.preventDefault();
					}
				});
			});

		panel.addEventListener('pointerdown', onPointerDown);
		panel.addEventListener('pointermove', onPointerMove);
		panel.addEventListener('pointerup', onPointerEnd);
		panel.addEventListener('pointercancel', onPointerEnd);
		panel.addEventListener('lostpointercapture', onPointerEnd);
	}

	// D-pad cell or fire button under the pointer -> { joy, x, y } / { joy, fire: true }, null = nothing
	function targetAt(clientX, clientY) {
		var el = document.elementFromPoint(clientX, clientY);
		if (!el || !el.getAttribute || el.getAttribute('data-joy') === null) {
			return null;
		}
		var joy = Number(el.getAttribute('data-joy'));
		if (el.getAttribute('data-fire')) {
			return { joy: joy, fire: true };
		}
		return { joy: joy, x: Number(el.getAttribute('data-x')), y: Number(el.getAttribute('data-y')) };
	}

	function onPointerDown(e) {
		e.stopPropagation();
		e.preventDefault();
		var t = targetAt(e.clientX, e.clientY);
		if (!t) {
			return;
		}
		pointers[e.pointerId] = t;
		try {
			panel.setPointerCapture(e.pointerId);		// moves and release come here even outside the panel
		} catch (err) {}
		clicksChanged();
	}

	function onPointerMove(e) {
		var p = pointers[e.pointerId];
		if (!p || p.fire) {			// fire stays pressed until released
			return;
		}
		var t = targetAt(e.clientX, e.clientY);
		if (t && !t.fire && t.joy === p.joy && (t.x !== p.x || t.y !== p.y)) {	// slid to another direction of the same joystick
			pointers[e.pointerId] = t;
			clicksChanged();
		}
	}

	function onPointerEnd(e) {
		if (pointers[e.pointerId]) {
			delete pointers[e.pointerId];
			clicksChanged();
		}
	}

	// state of the panel clicks from the held pointers -> send what changed, show it
	function clicksChanged() {
		for (var j = 0; j < 2; j++) {
			var x = 0, y = 0, fire = false;
			Object.keys(pointers).forEach(function(id) {
				var p = pointers[id];
				if (p.joy !== j) {
					return;
				}
				if (p.fire) {
					fire = true;
				} else {
					x = p.x;		// the last pointer on the D-pad wins
					y = p.y;
				}
			});

			var s = sent[j];
			if (x !== s.x) {
				s.x = x;
				post({ type: 'axis', joy: j, axis: 0, value: x * AXIS_MAX });
			}
			if (y !== s.y) {
				s.y = y;
				post({ type: 'axis', joy: j, axis: 1, value: y * AXIS_MAX });
			}
			if (fire !== s.fire) {
				s.fire = fire;
				post({ type: 'button', joy: j, button: 0, state: fire ? 'down' : 'up' });
			}
			show('mouse', j, x, y, fire);
		}
	}

	function releaseClicks() {
		if (Object.keys(pointers).length) {
			pointers = {};
			clicksChanged();
		}
	}

	// light up the D-pad cell / fire of what all sources hold now
	function render() {
		if (!panel) {
			return;
		}
		for (var j = 0; j < 2; j++) {
			var x = 0, y = 0, fire = false;
			SOURCES.forEach(function(src) {
				var s = state[j][src];
				if (!s) {
					return;
				}
				x = x || s.x;
				y = y || s.y;
				fire = fire || s.fire;
			});
			var cells = panel.querySelectorAll('[data-joy="' + j + '"]');
			for (var i = 0; i < cells.length; i++) {
				var el = cells[i];
				var on = el.getAttribute('data-fire') ? fire
					: (Number(el.getAttribute('data-x')) === x && Number(el.getAttribute('data-y')) === y);
				el.classList.toggle('on', on);
			}
		}
	}

	function show(source, joy, x, y, fire) {
		if (joy !== 0 && joy !== 1) {
			return;
		}
		state[joy][source] = { x: x || 0, y: y || 0, fire: !!fire };
		render();
	}

	function updateButton() {
		if (!button) {
			return;
		}
		button.classList.toggle('pressed', shown);
		button.textContent = shown ? 'Hide joysticks' : 'Show joysticks';
	}

	function toggle() {
		shown = !shown;
		document.body.classList.toggle('joypanel-shown', shown);
		if (!shown) {
			releaseClicks();
		}
		updateButton();
	}

	function bindButton() {
		button = document.getElementById('showjoysticks');
		if (!button) {
			return;
		}
		// toggled on press, like the other buttons of the page (script.js bindClickHelper) - and not an ST click
		var onPress = function(e) {
			e.preventDefault();
			e.stopPropagation();
			toggle();
			button.blur();
		};
		var ignore = function(e) {
			e.preventDefault();
			e.stopPropagation();
		};
		button.addEventListener('mousedown', onPress);
		button.addEventListener('touchstart', onPress);
		['mouseup', 'click', 'touchend'].forEach(function(name) { button.addEventListener(name, ignore); });
		updateButton();
	}

	document.addEventListener('DOMContentLoaded', function() {
		build();
		bindButton();
		render();
	});
	window.addEventListener('blur', releaseClicks);		// release goes to another window - don't leave it held on the ST

	return {
		// source: 'mouse' / 'keys' / 'pad', joy 0 / 1, x / y -1 .. 1 (- = left / up), fire true / false
		show: show
	};
})();
