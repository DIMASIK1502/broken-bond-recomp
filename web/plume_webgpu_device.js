// Link with --pre-js for plume's WebGPU backend. Requesting a WebGPU device is asynchronous, so this does it before
// main() runs and hands the device over as Module.preinitializedWebGPUDevice, which emscripten_webgpu_get_device()
// returns. Optional features plume can use are asked for when the adapter has them, and the adapter's own maximum for
// the limits a renderer with big descriptor sets and buffers runs into first (WebGPU's defaults are far below what
// desktop GPUs offer), and for the largest texture, which a renderer scaling its targets to the screen runs into (8192
// by default, where D3D12 and most adapters allow 16384). Validation errors and a lost device are logged to the
// console. The adapter's info (vendor, architecture) is kept as Module.plumeWebGPUAdapterInfo. At the end,
// setBindGroup is made to work in Firefox with a heap past 2 GB.
//
// A device is lost to a driver update or reset, the browser's GPU process crashing or a laptop switching GPUs. Then a
// new one is asked for the same way and put in the old one's place, emscripten_webgpu_get_device() returning it from
// then on, and the old one's plumeReplaced promise resolves: plume's device made on it says isLost(), and the
// application makes everything again on a new one. A device lost within 10 s of being made isn't replaced, since
// another would likely go the same way, nor one whose replacement couldn't be had: then the page's
// Module.plumeWebGPUDeviceLostForGood(info), if it has one, tells the player (plumeReplaced never settles).
;(() => {
  // The generated preamble ends on the same line as this file. The leading
  // semicolon keeps this from being parsed as a call on that line.
  if (ENVIRONMENT_IS_WORKER) return;
  const requestDevice = async () => {
    if (!navigator.gpu) {
      throw new Error('This browser has no WebGPU (navigator.gpu is missing; the page must be a secure context).');
    }

    const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    if (!adapter) {
      throw new Error('No WebGPU adapter.');
    }

    const optionalFeatures = ['float32-filterable', 'float32-blendable', 'depth-clip-control', 'dual-source-blending', 'texture-compression-bc', 'timestamp-query',
      'depth32float-stencil8'];
    const requiredFeatures = optionalFeatures.filter((feature) => adapter.features.has(feature));
    const raisedLimits = ['maxSampledTexturesPerShaderStage', 'maxSamplersPerShaderStage', 'maxStorageBuffersPerShaderStage',
      'maxStorageTexturesPerShaderStage', 'maxBindingsPerBindGroup', 'maxStorageBufferBindingSize', 'maxBufferSize',
      'maxTextureDimension2D'];
    const requiredLimits = {};
    for (const limit of raisedLimits) {
      if (adapter.limits[limit] !== undefined) {
        requiredLimits[limit] = adapter.limits[limit];
      }
    }

    const device = await adapter.requestDevice({ requiredFeatures, requiredLimits });
    const madeAt = performance.now();
    device.addEventListener('uncapturederror', (event) => console.error('WebGPU error: ' + event.error.message));
    device.plumeReplaced = device.lost.then(async (info) => {
      console.error('WebGPU device lost: ' + info.message);
      try {
        if (performance.now() - madeAt < 10000) {
          throw new Error('plume: the WebGPU device was lost ' + ((performance.now() - madeAt) / 1000).toFixed(1) + ' s after it was made, so it is not replaced');
        }

        const replacement = await requestDevice();
        Module['preinitializedWebGPUDevice'] = replacement.device;
        Module['plumeWebGPUAdapterInfo'] = replacement.adapter.info;
        // emscripten_webgpu_get_device() imports the device once and keeps it; this has it import the new one.
        WebGPU.preinitializedDeviceId = undefined;
        console.log('plume: a new WebGPU device in place of the lost one');
      } catch (error) {
        console.error(String(error));
        if (Module['plumeWebGPUDeviceLostForGood']) {
          Module['plumeWebGPUDeviceLostForGood'](info);
        }
        await new Promise(() => {});
      }
    });

    return { adapter, device };
  };

  Module['preRun'] = [].concat(Module['preRun'] || []);
  Module['preRun'].push(() => {
    addRunDependency('plume-webgpu-device');
    // The first requestAdapter in this browser can stay pending forever. Ask
    // again until one settles, and ignore a late result from an earlier try.
    let settled = false;
    const finish = ({ adapter, device }) => {
      if (settled) return;
      settled = true;
      Module['preinitializedWebGPUDevice'] = device;
      Module['plumeWebGPUAdapterInfo'] = adapter.info;
      removeRunDependency('plume-webgpu-device');
    };
    const fail = (error) => {
      if (settled) return;
      console.error(error);
      abort(error);
    };
    const attempt = () => {
      if (settled) return;
      requestDevice().then(finish, fail);
      setTimeout(attempt, 1000);
    };
    setTimeout(attempt, 0);
  });
})();

// emdawnwebgpu hands setBindGroup its dynamic offsets as the whole wasm heap (HEAPU32) with a start and a length.
// Firefox refuses any ArrayBuffer or view larger than 2 GB as an argument, so on a page whose heap has grown past that
// every bind group with dynamic offsets threw and nothing was drawn. There a view that big is replaced by the offsets
// copied into an array kept for it; any heap under 2 GB takes the call as before. Chrome takes the whole heap, and
// its pages call the browser's own setBindGroup: RT64 binds a group with an offset for every draw, and a wrapper with
// a copy took ~1.4 us a call in Chrome 155 against 0.13 straight (a 3 GB heap; 0.48 under 2 GB), ~9% of the page's
// thread in Conker's Bad Fur Day with that thread slowed 3 times. (writeBuffer and writeTexture already pass a
// subarray.)
if (navigator.userAgent.includes('Firefox')) {
  const copied = new Uint32Array(64);
  for (const name of ['GPUComputePassEncoder', 'GPURenderPassEncoder', 'GPURenderBundleEncoder']) {
    const encoder = globalThis[name];
    if (!encoder) {
      continue;
    }

    const setBindGroup = encoder.prototype.setBindGroup;
    encoder.prototype.setBindGroup = function (index, bindGroup, offsets, start, length) {
      if (offsets && (offsets.byteLength > 0x7fffffff) && (length <= copied.length)) {
        for (let i = 0; i < length; i++) {
          copied[i] = offsets[start + i];
        }

        return setBindGroup.call(this, index, bindGroup, copied, 0, length);
      }

      if (offsets && (offsets.byteLength > 0x7fffffff)) {
        return setBindGroup.call(this, index, bindGroup, offsets.slice(start, start + length));
      }

      return setBindGroup.apply(this, arguments);
    };
  }
}
