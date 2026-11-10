// Where the browser build reads the game. The page copies a local folder into
// OPFS before main runs; this mounts that store. Nothing is fetched.

#pragma once

#include <string>

namespace broken_bond::web {

// Mounts OPFS at /opfs and requires broken_bond/game/default.xex.
// On failure the page is told, and this returns false.
bool InitializeStorage();

// /opfs/broken_bond/game after a successful mount, otherwise empty.
const std::string& GameRoot();

}  // namespace broken_bond::web
