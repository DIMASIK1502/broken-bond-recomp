// Plays the ring in src/web/web_audio.h. Adapted from Trials HD Recompiled (MIT).
//
// The ring lives in the wasm SharedArrayBuffer: a uint32 header, then
// interleaved stereo float samples.
//   0 write_frame  4 read_frame  8 capacity_frames  12 sample_rate  16 channels
//   20 underruns  24 heartbeat  28 peak_milli  32 samples[capacity * 2]
// The driver writes and advances write_frame; this processor reads, advances
// read_frame, and bumps heartbeat every quantum so the driver knows it plays.

const kHeaderWords = 8;
const kWrite = 0;
const kRead = 1;
const kCapacity = 2;
const kUnderruns = 5;
const kHeartbeat = 6;
const kPeak = 7;
// A short quantum counts as an underrun only while the driver is writing
// (it wrote within this many quanta, ~53 ms), not when the game is silent.
const kFedQuanta = 20;

class BrokenBondAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.header = null;
    this.samples = null;
    this.lastWrite = 0;
    this.quantaSinceWrite = kFedQuanta;
    this.peak = 0;
    this.recording = null;
    this.port.onmessage = (event) => {
      if (event.data.record) {
        // Test/debug: copy the next `record` frames played (stereo, interleaved).
        this.recording = { data: new Float32Array(event.data.record * 2), frames: 0 };
        return;
      }
      const { buffer, offset } = event.data;
      this.capacity = new Uint32Array(buffer, offset, kHeaderWords)[kCapacity];
      this.header = new Uint32Array(buffer, offset, kHeaderWords);
      this.samples = new Float32Array(buffer, offset + kHeaderWords * 4, this.capacity * 2);
      this.lastWrite = Atomics.load(this.header, kWrite);
    };
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const left = out[0];
    const right = out.length > 1 ? out[1] : null;
    const frames = left.length;
    if (!this.header) {
      return true;
    }
    const header = this.header;
    const write = Atomics.load(header, kWrite);
    let read = Atomics.load(header, kRead);
    if (write !== this.lastWrite) {
      this.lastWrite = write;
      this.quantaSinceWrite = 0;
    } else if (this.quantaSinceWrite < kFedQuanta) {
      ++this.quantaSinceWrite;
    }
    const available = (write - read) >>> 0;
    const count = Math.min(available, frames);
    const capacity = this.capacity;
    const samples = this.samples;
    let peak = this.peak;
    for (let i = 0; i < count; ++i) {
      const index = ((read + i) % capacity) * 2;
      const l = samples[index];
      const r = samples[index + 1];
      if (right) {
        left[i] = l;
        right[i] = r;
      } else {
        left[i] = 0.5 * (l + r);
      }
      peak = Math.max(peak, Math.abs(l), Math.abs(r));
    }
    left.fill(0, count);
    if (right) {
      right.fill(0, count);
    }
    const rec = this.recording;
    if (rec) {
      const n = Math.min(frames, rec.data.length / 2 - rec.frames);
      for (let i = 0; i < n; ++i) {
        rec.data[(rec.frames + i) * 2] = left[i];
        rec.data[(rec.frames + i) * 2 + 1] = right ? right[i] : left[i];
      }
      rec.frames += n;
      if (rec.frames * 2 >= rec.data.length) {
        this.port.postMessage({ recorded: rec.data }, [rec.data.buffer]);
        this.recording = null;
      }
    }
    if (count < frames && this.quantaSinceWrite < kFedQuanta) {
      Atomics.add(header, kUnderruns, 1);
    }
    if (peak !== this.peak) {
      this.peak = peak;
      Atomics.store(header, kPeak, Math.min(1000, Math.round(peak * 1000)));
    }
    Atomics.store(header, kRead, (read + count) >>> 0);
    Atomics.add(header, kHeartbeat, 1);
    return true;
  }
}

registerProcessor('broken-bond-audio', BrokenBondAudioProcessor);
