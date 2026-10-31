// Parsed before other headers on the web core. xxHash's inline implementation
// can include Emscripten headers from inside extern "C", and em_asm.h's
// templates then have the wrong linkage. Including it first lets the include
// guard win.
#include <emscripten/em_asm.h>
