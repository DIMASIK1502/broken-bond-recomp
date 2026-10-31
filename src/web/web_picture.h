#pragma once

namespace broken_bond::web {

// Opens a WebGPU swapchain on #canvas. The guest swap hook then copies the
// frontbuffer onto it when that texture has texels.
bool StartPicture();

}  // namespace broken_bond::web
