// ------------------------------------------------------------------- overlay.h
//
// An on-screen feed for Archipelago events: checks sent, items received from
// other players. The game cannot show the second kind at all -- it has no
// concept of another player's world -- so this is the only place it can appear.
//
// Deliberately NOT a renderer hook. This is a layered, click-through, top-most
// window drawn with GDI on its own thread. It never touches D3D12, never runs
// on the game thread, and cannot corrupt a frame or crash the game: the worst
// failure is a window that does not appear.
#pragma once

// windows.h defines min/max as macros, which breaks std::min / std::max in the
// implementation. NOMINMAX must land before it is pulled in anywhere.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace overlay {

// Start / stop the feed. Safe to call repeatedly.
bool Start();
void Stop();
bool Running();

// Queue a line. Thread-safe; callable from the poll thread or a hook.
// `kind` picks the colour: 0 = check sent, 1 = item received, 2 = notice.
void Push(int kind, const char* text);

// --- the connect panel ----------------------------------------------------
//
// Signing in should happen from inside the game rather than a terminal. The
// overlay is normally click-through (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
// opening the panel drops those styles so it can take the keyboard, and
// closing it puts them back.
//
// The mod does not speak the Archipelago protocol -- the Python client does.
// So submitting writes the settings to a file the client watches, which keeps
// the network code where it already works.

// Open / close the panel. Safe from the poll thread.
void SetOffsets(int x, int y_from_bottom);
void SetEnergy(int percent);   // 0..100 shows the meter, -1 hides it

// Connection state for the indicator: a filled dot, green when connected.
void SetConnected(bool on, const char* detail);

}  // namespace overlay
