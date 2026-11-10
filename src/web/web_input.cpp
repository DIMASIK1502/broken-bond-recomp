#include "web/web_input.h"

namespace broken_bond::web {

WebInputBlock& InputBlock() {
  static WebInputBlock block{};
  return block;
}

}  // namespace broken_bond::web

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

extern "C" EMSCRIPTEN_KEEPALIVE uintptr_t broken_bond_web_input_block() {
  return reinterpret_cast<uintptr_t>(&broken_bond::web::InputBlock());
}
#endif
