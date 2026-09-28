// ui/Fonts.h — optional nicer typeface for the ImGui developer tools, loaded
// from the host OS. (The player-facing UI ships its own fonts in
// assets/fonts; see src/gui.)
#pragma once

namespace immune::ui {

/// Tries a short list of common system UI fonts (Segoe UI on Windows, San
/// Francisco/Helvetica on macOS, DejaVu/Liberation on Linux) and bakes it
/// into the current ImGui font atlas as the default font. Must be called
/// after ImGui::CreateContext() and before the render backend's *_Init()
/// builds the font atlas texture.
///
/// If no candidate font is found on disk, this is a no-op and ImGui keeps
/// its built-in pixel font -- the UI still works, just without the upgrade.
void load_system_fonts();

} // namespace immune::ui
