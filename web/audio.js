// Starts playback of the wasm audio ring (src/web/web_audio.h).
// Adapted from Trials HD Recompiled (MIT). The AudioContext stays suspended
// until a key, click or touch; until then the driver drops frames and does
// not block the game.

const kHeaderWords = 8;

export class WebAudio {
  /**
   * @param {object} module the Emscripten module (wasmMemory, _broken_bond_web_audio_ring)
   */
  constructor(module) {
    this.module = module;
    this.context = null;
    this.node = null;
    this.header = null;
    this.starting = null;
    this._onGesture = () => this.resume();
  }

  // Creates the context (suspended without a gesture) and the worklet, and
  // resumes on the first gesture. Returns once the worklet is connected.
  async start() {
    if (this.starting) {
      return this.starting;
    }
    this.starting = (async () => {
      const buffer = this.module.wasmMemory.buffer;
      const offset = Number(this.module._broken_bond_web_audio_ring());
      this.header = new Uint32Array(buffer, offset, kHeaderWords);
      this.context = new AudioContext({ sampleRate: this.header[3] || 48000,
                                        latencyHint: 'interactive' });
      await this.context.audioWorklet.addModule(new URL('./audio_worklet.js', import.meta.url));
      this.node = new AudioWorkletNode(this.context, 'broken-bond-audio', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [2],
      });
      this.node.port.postMessage({ buffer, offset });
      this.node.connect(this.context.destination);
      for (const type of ['keydown', 'pointerdown', 'touchend']) {
        window.addEventListener(type, this._onGesture, { capture: true });
      }
      await this.resume();
    })();
    return this.starting;
  }

  async resume() {
    if (!this.context || this.context.state === 'running') {
      return;
    }
    try {
      await this.context.resume();
    } catch (error) {
      // Not allowed yet (no gesture): stays suspended until the next one.
    }
    if (this.context.state === 'running') {
      for (const type of ['keydown', 'pointerdown', 'touchend']) {
        window.removeEventListener(type, this._onGesture, { capture: true });
      }
    }
  }

  // For tests and debugging: resolves with the next `seconds` of what the
  // worklet plays, as interleaved stereo samples at the context's rate.
  record(seconds) {
    return new Promise((resolve) => {
      this.node.port.onmessage = (event) => {
        if (event.data.recorded) {
          resolve(event.data.recorded);
        }
      };
      this.node.port.postMessage({ record: Math.round(seconds * this.context.sampleRate) });
    });
  }

  // For tests and the console: the ring's counters.
  stats() {
    const h = this.header;
    if (!h) {
      return null;
    }
    return {
      state: this.context ? this.context.state : 'none',
      writeFrame: Atomics.load(h, 0),
      readFrame: Atomics.load(h, 1),
      underruns: Atomics.load(h, 5),
      heartbeat: Atomics.load(h, 6),
      peak: Atomics.load(h, 7) / 1000,
    };
  }

  stop() {
    if (this.context) {
      this.context.close();
    }
  }
}
