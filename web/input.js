/**
 * Main-thread input for the Broken Bond page. Adapted from Trials HD
 * Recompiled (MIT). Layout is src/web/web_input.h.
 * - Polls Gamepad API (standard mapping) with seqlock synchronization.
 * - Captures keyboard events (KeyboardEvent.code mapped to SDL scancodes).
 * - Captures pointer/mouse events and wheel for ImGui UI drawers.
 * - Writes events to the lock-free ring buffer in WebInputBlock.
 */

// SDL Scancode mapping from W3C KeyboardEvent.code
export const CODE_TO_SCANCODE = {
  // Letters
  KeyA: 4, KeyB: 5, KeyC: 6, KeyD: 7, KeyE: 8, KeyF: 9, KeyG: 10,
  KeyH: 11, KeyI: 12, KeyJ: 13, KeyK: 14, KeyL: 15, KeyM: 16, KeyN: 17,
  KeyO: 18, KeyP: 19, KeyQ: 20, KeyR: 21, KeyS: 22, KeyT: 23, KeyU: 24,
  KeyV: 25, KeyW: 26, KeyX: 27, KeyY: 28, KeyZ: 29,

  // Digits
  Digit1: 30, Digit2: 31, Digit3: 32, Digit4: 33, Digit5: 34,
  Digit6: 35, Digit7: 36, Digit8: 37, Digit9: 38, Digit0: 39,

  // Controls & Whitespace
  Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44,
  Minus: 45, Equal: 46, BracketLeft: 47, BracketRight: 48, Backslash: 49,
  Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55, Slash: 56,
  CapsLock: 57,

  // Function keys
  F1: 58, F2: 59, F3: 60, F4: 61, F5: 62, F6: 63,
  F7: 64, F8: 65, F9: 66, F10: 67, F11: 68, F12: 69,

  // Navigation & Editing
  PrintScreen: 70, ScrollLock: 71, Pause: 72, Insert: 73, Home: 74,
  PageUp: 75, Delete: 76, End: 77, PageDown: 78,
  ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82,

  // Keypad
  NumLock: 83, NumpadDivide: 84, NumpadMultiply: 85, NumpadSubtract: 86, NumpadAdd: 87,
  NumpadEnter: 88, Numpad1: 89, Numpad2: 90, Numpad3: 91, Numpad4: 92, Numpad5: 93,
  Numpad6: 94, Numpad7: 95, Numpad8: 96, Numpad9: 97, Numpad0: 98,
  NumpadDecimal: 99,

  // Modifiers
  ControlLeft: 224, ShiftLeft: 225, AltLeft: 226, MetaLeft: 227,
  ControlRight: 228, ShiftRight: 229, AltRight: 230, MetaRight: 231,
};

// Event Types (WebInputEvent.type)
export const EVENT_TYPE = {
  KEY_DOWN: 1,
  KEY_UP: 2,
  POINTER_MOVE: 3,
  POINTER_DOWN: 4,
  POINTER_UP: 5,
  WHEEL: 6,
  TEXT: 7,
  FOCUS: 8,
  BLUR: 9,
};

// Pointer Button Mapping (SDL button numbering: 1=left, 2=middle, 3=right)
export const MOUSE_BUTTON = {
  0: 1, // Left
  1: 2, // Middle
  2: 3, // Right
  3: 4, // X1 / Back
  4: 5, // X2 / Forward
};

export class WebInput {
  /**
   * @param {HTMLCanvasElement} canvas
   * @param {SharedArrayBuffer} sab
   * @param {number} byteOffset Offset of WebInputBlock in SAB
   */
  constructor(canvas, sab, byteOffset = 0) {
    this.canvas = canvas;
    this.sab = sab;
    this.byteOffset = byteOffset;

    // Byte offsets inside WebInputBlock. The views start at the block, so
    // indices stay small: with Memory64 the block can lie above 2 GiB, where
    // `offset >> 2` on the whole buffer would turn negative.
    this.OFF_PAD_SEQ = 0;
    this.OFF_PAD_COUNT = 4;
    this.OFF_PADS = 8; // 4 * 32 bytes = 128 bytes
    this.OFF_EVENT_HEAD = 136;
    this.OFF_EVENT_TAIL = 140;
    this.OFF_EVENTS = 144; // 256 * 16 bytes = 4096 bytes
    this.OFF_CANVAS_WIDTH = 4240;
    this.OFF_CANVAS_HEIGHT = 4244;
    this.OFF_DPR = 4248;
    this.OFF_VISIBLE = 4252;
    const kBlockWords = 4256 / 4;

    this.u32 = new Uint32Array(sab, byteOffset, kBlockWords);
    this.i32 = new Int32Array(sab, byteOffset, kBlockWords);
    this.f32 = new Float32Array(sab, byteOffset, kBlockWords);

    // A scripted pad merged into pad 0 (tools/web/cdp.py --script drives it
    // through window.setSyntheticPadButton / setSyntheticPadAxis).
    this.synthetic = {
      active: false,
      buttons: 0,
      axes: [0.0, 0.0, 0.0, 0.0],
      triggers: [0.0, 0.0],
    };

    this.running = false;
    this.rafId = null;

    // Bound listeners for cleanup
    this._onKeyDown = this.handleKeyDown.bind(this);
    this._onKeyUp = this.handleKeyUp.bind(this);
    this._onPointerMove = this.handlePointerMove.bind(this);
    this._onPointerDown = this.handlePointerDown.bind(this);
    this._onPointerUp = this.handlePointerUp.bind(this);
    this._onWheel = this.handleWheel.bind(this);
    this._onFocus = this.handleFocus.bind(this);
    this._onBlur = this.handleBlur.bind(this);
    this._onVisibilityChange = this.updateVisibility.bind(this);
    this._onResize = this.updateDimensions.bind(this);
    this._onContextMenu = (event) => event.preventDefault();
    this._onDoubleClick = () => this.toggleFullscreen();
    this._pollLoop = this.pollLoop.bind(this);
  }

  start() {
    if (this.running) return;
    this.running = true;

    // Event listeners
    window.addEventListener('keydown', this._onKeyDown, { passive: false });
    window.addEventListener('keyup', this._onKeyUp, { passive: false });

    if (this.canvas) {
      this.canvas.addEventListener('pointermove', this._onPointerMove, { passive: true });
      this.canvas.addEventListener('pointerdown', this._onPointerDown, { passive: false });
      this.canvas.addEventListener('pointerup', this._onPointerUp, { passive: false });
      this.canvas.addEventListener('wheel', this._onWheel, { passive: false });
      this.canvas.addEventListener('contextmenu', this._onContextMenu);
      this.canvas.addEventListener('dblclick', this._onDoubleClick);
    }

    window.addEventListener('focus', this._onFocus);
    window.addEventListener('blur', this._onBlur);
    document.addEventListener('visibilitychange', this._onVisibilityChange);
    window.addEventListener('resize', this._onResize);

    // Initial state setup
    this.updateDimensions();
    this.updateVisibility();

    // Start rAF polling loop
    this.rafId = requestAnimationFrame(this._pollLoop);
  }

  stop() {
    if (!this.running) return;
    this.running = false;

    if (this.rafId) {
      cancelAnimationFrame(this.rafId);
      this.rafId = null;
    }

    window.removeEventListener('keydown', this._onKeyDown);
    window.removeEventListener('keyup', this._onKeyUp);

    if (this.canvas) {
      this.canvas.removeEventListener('pointermove', this._onPointerMove);
      this.canvas.removeEventListener('pointerdown', this._onPointerDown);
      this.canvas.removeEventListener('pointerup', this._onPointerUp);
      this.canvas.removeEventListener('wheel', this._onWheel);
      this.canvas.removeEventListener('contextmenu', this._onContextMenu);
      this.canvas.removeEventListener('dblclick', this._onDoubleClick);
    }

    window.removeEventListener('focus', this._onFocus);
    window.removeEventListener('blur', this._onBlur);
    document.removeEventListener('visibilitychange', this._onVisibilityChange);
    window.removeEventListener('resize', this._onResize);
  }

  async toggleFullscreen() {
    try {
      if (document.fullscreenElement) await document.exitFullscreen();
      else await this.canvas.requestFullscreen();
    } catch (error) {
      console.warn('Fullscreen unavailable:', error);
    }
  }

  updateDimensions() {
    if (!this.canvas) return;
    const dpr = window.devicePixelRatio || 1.0;
    const width = Math.round(this.canvas.clientWidth * dpr);
    const height = Math.round(this.canvas.clientHeight * dpr);

    Atomics.store(this.u32, this.OFF_CANVAS_WIDTH >> 2, width);
    Atomics.store(this.u32, this.OFF_CANVAS_HEIGHT >> 2, height);
    this.f32[this.OFF_DPR >> 2] = dpr;
  }

  updateVisibility() {
    const isVisible = document.visibilityState === 'visible' ? 1 : 0;
    Atomics.store(this.u32, this.OFF_VISIBLE >> 2, isVisible);
  }

  pushEvent(type, code, x, y) {
    const head = Atomics.load(this.u32, this.OFF_EVENT_HEAD >> 2);
    const tail = Atomics.load(this.u32, this.OFF_EVENT_TAIL >> 2);

    // Ring buffer size: 256
    if (((head - tail) >>> 0) >= 256) {
      // Ring full: drop oldest event by advancing tail or drop newest
      return false;
    }

    const slot = head & 255;
    const eventBase = this.OFF_EVENTS + slot * 16;
    const u32Idx = eventBase >> 2;

    this.u32[u32Idx + 0] = type >>> 0;
    this.u32[u32Idx + 1] = code >>> 0;
    this.i32[u32Idx + 2] = x | 0;
    this.i32[u32Idx + 3] = y | 0;

    // Advance head atomically with release semantics
    Atomics.store(this.u32, this.OFF_EVENT_HEAD >> 2, (head + 1) >>> 0);
    return true;
  }

  handleKeyDown(e) {
    // Avoid interfering with browser shortcuts like F12, Ctrl+Shift+I, Ctrl+R
    if (e.key === 'F12' || (e.ctrlKey && e.key === 'r') || (e.ctrlKey && e.shiftKey && e.key === 'I')) {
      return;
    }

    const scancode = CODE_TO_SCANCODE[e.code] || 0;
    this.pushEvent(EVENT_TYPE.KEY_DOWN, scancode, 0, 0);

    // If printable character, push text event for ImGui text input
    if (e.key && e.key.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
      const codePoint = e.key.codePointAt(0);
      this.pushEvent(EVENT_TYPE.TEXT, codePoint, 0, 0);
    }

    // Prevent default scrolling for game keys
    if (['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Space'].includes(e.code)) {
      e.preventDefault();
    }
  }

  handleKeyUp(e) {
    const scancode = CODE_TO_SCANCODE[e.code] || 0;
    this.pushEvent(EVENT_TYPE.KEY_UP, scancode, 0, 0);
  }

  getCanvasCoords(e) {
    if (!this.canvas) return { x: 0, y: 0 };
    const rect = this.canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1.0;
    // The GPU worker owns the transferred canvas; DOM width/height can be stale.
    const scaleX = (Atomics.load(this.u32, this.OFF_CANVAS_WIDTH >> 2) / rect.width) || dpr;
    const scaleY = (Atomics.load(this.u32, this.OFF_CANVAS_HEIGHT >> 2) / rect.height) || dpr;
    return {
      x: Math.round((e.clientX - rect.left) * scaleX),
      y: Math.round((e.clientY - rect.top) * scaleY),
    };
  }

  handlePointerMove(e) {
    const { x, y } = this.getCanvasCoords(e);
    this.pushEvent(EVENT_TYPE.POINTER_MOVE, e.buttons, x, y);
  }

  handlePointerDown(e) {
    const { x, y } = this.getCanvasCoords(e);
    const button = MOUSE_BUTTON[e.button] || 1;
    this.canvas?.setPointerCapture(e.pointerId);
    this.pushEvent(EVENT_TYPE.POINTER_DOWN, button, x, y);
    if (this.canvas && document.activeElement !== this.canvas) {
      this.canvas.focus();
    }
  }

  handlePointerUp(e) {
    const { x, y } = this.getCanvasCoords(e);
    const button = MOUSE_BUTTON[e.button] || 1;
    this.pushEvent(EVENT_TYPE.POINTER_UP, button, x, y);
  }

  handleWheel(e) {
    const { x, y } = this.getCanvasCoords(e);
    // Standard delta scaling: 120 units per notch
    const deltaX = Math.round(e.deltaX);
    const deltaY = Math.round(e.deltaY);
    this.pushEvent(EVENT_TYPE.POINTER_MOVE, e.buttons || 0, x, y);
    this.pushEvent(EVENT_TYPE.WHEEL, 0, deltaX, deltaY);
    e.preventDefault();
  }

  handleFocus() {
    this.pushEvent(EVENT_TYPE.FOCUS, 0, 0, 0);
  }

  handleBlur() {
    this.pushEvent(EVENT_TYPE.BLUR, 0, 0, 0);
  }

  pollGamepads() {
    const rawPads = navigator.getGamepads ? navigator.getGamepads() : [];
    let count = 0;

    // Seqlock: increment pad_seq to odd number to indicate update in progress
    const seq = Atomics.load(this.u32, this.OFF_PAD_SEQ >> 2);
    Atomics.store(this.u32, this.OFF_PAD_SEQ >> 2, (seq + 1) >>> 0);

    for (let i = 0; i < 4; ++i) {
      const pad = rawPads[i];
      const padBase = this.OFF_PADS + i * 32;
      const u32Base = padBase >> 2;
      const f32Base = padBase >> 2;

      if (pad && pad.connected) {
        count++;
        // connected = 1
        this.u32[u32Base + 0] = 1;

        // buttons bitmask (bit i = pad.buttons[i].pressed)
        let btnMask = 0;
        const btnCount = Math.min(pad.buttons.length, 17);
        for (let b = 0; b < btnCount; ++b) {
          if (pad.buttons[b].pressed) {
            btnMask |= (1 << b);
          }
        }
        this.u32[u32Base + 1] = btnMask >>> 0;

        // axes: LX, LY, RX, RY (-1.0 .. 1.0)
        this.f32[f32Base + 2] = pad.axes[0] !== undefined ? pad.axes[0] : 0.0;
        this.f32[f32Base + 3] = pad.axes[1] !== undefined ? pad.axes[1] : 0.0;
        this.f32[f32Base + 4] = pad.axes[2] !== undefined ? pad.axes[2] : 0.0;
        this.f32[f32Base + 5] = pad.axes[3] !== undefined ? pad.axes[3] : 0.0;

        // triggers: LT (buttons[6]), RT (buttons[7])
        const lt = pad.buttons[6] ? pad.buttons[6].value : 0.0;
        const rt = pad.buttons[7] ? pad.buttons[7].value : 0.0;
        this.f32[f32Base + 6] = lt;
        this.f32[f32Base + 7] = rt;
      } else {
        // Disconnected
        this.u32[u32Base + 0] = 0;
        this.u32[u32Base + 1] = 0;
        this.f32[f32Base + 2] = 0.0;
        this.f32[f32Base + 3] = 0.0;
        this.f32[f32Base + 4] = 0.0;
        this.f32[f32Base + 5] = 0.0;
        this.f32[f32Base + 6] = 0.0;
        this.f32[f32Base + 7] = 0.0;
      }
    }

    if (this.synthetic.active) {
      const base = this.OFF_PADS >> 2;
      if (!this.u32[base + 0]) {
        count++;
        this.u32[base + 1] = 0;
        for (let k = 2; k < 8; ++k) this.f32[base + k] = 0.0;
      }
      this.u32[base + 0] = 1;
      this.u32[base + 1] = (this.u32[base + 1] | this.synthetic.buttons) >>> 0;
      for (let k = 0; k < 4; ++k) {
        if (this.synthetic.axes[k] !== 0.0) this.f32[base + 2 + k] = this.synthetic.axes[k];
      }
      for (let k = 0; k < 2; ++k) {
        this.f32[base + 6 + k] = Math.max(this.f32[base + 6 + k], this.synthetic.triggers[k]);
      }
    }

    Atomics.store(this.u32, this.OFF_PAD_COUNT >> 2, count);

    // Seqlock: increment pad_seq to next even number to indicate update complete
    Atomics.store(this.u32, this.OFF_PAD_SEQ >> 2, (seq + 2) >>> 0);
  }

  // Standard-mapping button index (0 = A ... 15 = D-pad right), pressed or not.
  setSyntheticButton(index, pressed) {
    this.synthetic.active = true;
    if (pressed) {
      this.synthetic.buttons |= (1 << index);
    } else {
      this.synthetic.buttons &= ~(1 << index);
    }
    if (index === 6 || index === 7) {
      this.synthetic.triggers[index - 6] = pressed ? 1.0 : 0.0;
    }
    this.pollGamepads();
  }

  // SDL raw axis values (as tests/headless scripts use): lx/ly/rx/ry in
  // -32768..32767 (+y = down, as the Gamepad API), lt/rt in 0..32767.
  setSyntheticAxis(name, raw) {
    this.synthetic.active = true;
    const value = Math.max(-1.0, Math.min(1.0, raw / 32767.0));
    const axisIndex = { lx: 0, ly: 1, rx: 2, ry: 3 }[name];
    if (axisIndex !== undefined) {
      this.synthetic.axes[axisIndex] = value;
    } else if (name === 'lt' || name === 'rt') {
      this.synthetic.triggers[name === 'lt' ? 0 : 1] = Math.max(0.0, value);
    }
    this.pollGamepads();
  }

  pollLoop() {
    if (!this.running) return;
    this.pollGamepads();
    this.updateDimensions();
    this.rafId = requestAnimationFrame(this._pollLoop);
  }
}

// Global attachment for non-module usage
if (typeof window !== 'undefined') {
  window.WebInput = WebInput;
  window.CODE_TO_SCANCODE = CODE_TO_SCANCODE;
  window.EVENT_TYPE = EVENT_TYPE;
}
