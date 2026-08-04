// render/Shader.h — GLSL program management with hot reload. FROZEN CONTRACT.
// Owner: Wave 1C.
//
// All shaders live in assets/shaders/*.glsl as plain text. There are no binary
// assets in this project (a locked decision), so shader source is also the
// project's *only* on-disk art. Hot reload matters accordingly: it is the whole
// art iteration loop.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::render {

/// Opaque GL program handle. 0 is invalid.
struct ShaderProgram {
    u32 gl_id = 0;
    bool valid() const { return gl_id != 0; }
};

class ShaderManager {
public:
    ~ShaderManager();

    /// Loads a vertex+fragment program from assets/shaders/. `name` is the key
    /// used for reload and lookup. Paths are relative to the shader directory.
    ShaderProgram load_graphics(const std::string& name,
                                const std::string& vert_path,
                                const std::string& frag_path);

    /// Loads a compute program (used by the optional GPU chaff-update path).
    ShaderProgram load_compute(const std::string& name, const std::string& comp_path);

    ShaderProgram get(const std::string& name) const;

    /// Recompiles any program whose source files changed on disk. A failed
    /// recompile logs the GLSL error and KEEPS the previous working program, so
    /// a typo never blanks the screen mid-iteration.
    /// Returns the number of programs successfully reloaded.
    u32 poll_reload();

    /// Last compile/link error, for tests to assert on.
    const std::string& last_error() const { return last_error_; }

    void clear();

    /// A `#define` prepended to every subsequent compile. Used for feature
    /// permutations (e.g. blob LOD on/off) without duplicating source files.
    void set_define(const std::string& key, const std::string& value);

private:
    struct Entry;
    std::vector<Entry*> entries_;
    std::vector<std::pair<std::string, std::string>> defines_;
    std::string last_error_;
};

} // namespace immune::render
