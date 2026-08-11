// ui/Fonts.h — optional nicer typeface for the ImGui UI, loaded from the
// host OS rather than shipped in the repo (this project ships zero binary
// assets -- see docs/ARCHITECTURE.md -- so no .ttf lives on disk here).
#pragma once

struct ImFont;

namespace immune::ui {

/// Tries a short list of common system UI fonts (Segoe UI on Windows, San
/// Francisco/Helvetica on macOS, DejaVu/Liberation on Linux) and bakes two
/// sizes into the current ImGui font atlas: a body size used as the default
/// font, and a larger display size for titles (see title_font()). Must be
/// called after ImGui::CreateContext() and before the render backend's
/// *_Init() builds the font atlas texture.
///
/// If no candidate font is found on disk, this is a no-op and ImGui keeps
/// its built-in pixel font -- the UI still works, just without the upgrade.
void load_system_fonts();

/// The larger display-size font baked by load_system_fonts(), for headings
/// (main menu title, "Level Complete", etc). Returns nullptr if no system
/// font was found, in which case callers should fall back to scaling
/// ImGui::GetFont() as before.
ImFont* title_font();

} // namespace immune::ui
