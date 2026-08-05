// render/Shader.cpp — GLSL program management with hot reload. Owner: Wave 1C.
//
// Shader source is the project's only on-disk art (no binary assets), so a
// typo during iteration must never blank the screen: a failed recompile logs
// the error and keeps whatever program last linked successfully.
#include "render/Shader.h"

#include "core/Log.h"
#include "platform/FileIO.h"

#include <glad/glad.h>

namespace immune::render {
namespace {

/// `load_graphics`/`load_compute` document their paths as relative to the
/// shader directory (assets/shaders/), not the asset root generally.
std::string shader_path(const std::string& relative) {
    return platform::asset_path("shaders/" + relative);
}

std::string with_defines(const std::string& source,
                         const std::vector<std::pair<std::string, std::string>>& defines) {
    if (defines.empty()) return source;
    // GLSL requires #version to be the first token in the file, so defines are
    // spliced in right after the first line rather than prepended outright.
    const usize nl = source.find('\n');
    const std::string first_line = nl == std::string::npos ? source : source.substr(0, nl + 1);
    const std::string rest = nl == std::string::npos ? std::string() : source.substr(nl + 1);
    std::string out = first_line;
    for (const auto& kv : defines) {
        out += "#define " + kv.first + " " + kv.second + "\n";
    }
    out += rest;
    return out;
}

/// Compiles one stage. Returns 0 and fills `out_error` on failure.
GLuint compile_stage(GLenum stage, const std::string& source, std::string& out_error) {
    const GLuint id = glCreateShader(stage);
    const char* src = source.c_str();
    const GLint len = static_cast<GLint>(source.size());
    glShaderSource(id, 1, &src, &len);
    glCompileShader(id);
    GLint ok = GL_FALSE;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint log_len = 0;
        glGetShaderiv(id, GL_INFO_LOG_LENGTH, &log_len);
        std::string log(static_cast<usize>(log_len > 0 ? log_len : 0), '\0');
        if (log_len > 0) glGetShaderInfoLog(id, log_len, nullptr, log.data());
        out_error = log;
        glDeleteShader(id);
        return 0;
    }
    return id;
}

GLuint link_program(GLuint vs, GLuint fs, std::string& out_error) {
    const GLuint prog = glCreateProgram();
    if (vs != 0) glAttachShader(prog, vs);
    if (fs != 0) glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint log_len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &log_len);
        std::string log(static_cast<usize>(log_len > 0 ? log_len : 0), '\0');
        if (log_len > 0) glGetProgramInfoLog(prog, log_len, nullptr, log.data());
        out_error = log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

} // namespace

struct ShaderManager::Entry {
    std::string name;
    bool is_compute = false;
    std::string vert_path, frag_path, comp_path;
    std::string last_vert_src, last_frag_src, last_comp_src;
    ShaderProgram program;
};

ShaderManager::~ShaderManager() { clear(); }

ShaderProgram ShaderManager::load_graphics(const std::string& name,
                                           const std::string& vert_path,
                                           const std::string& frag_path) {
    auto* e = new Entry{};
    e->name = name;
    e->vert_path = vert_path;
    e->frag_path = frag_path;

    const auto vsrc = platform::read_text_file(shader_path(vert_path));
    const auto fsrc = platform::read_text_file(shader_path(frag_path));
    if (!vsrc || !fsrc) {
        last_error_ = "shader '" + name + "': failed to read " +
                     (!vsrc ? vert_path : frag_path);
        IMMUNE_LOG_ERROR("%s", last_error_.c_str());
        entries_.push_back(e);
        return ShaderProgram{};
    }
    e->last_vert_src = *vsrc;
    e->last_frag_src = *fsrc;

    std::string verr, ferr, lerr;
    const GLuint vs = compile_stage(GL_VERTEX_SHADER, with_defines(*vsrc, defines_), verr);
    const GLuint fs = compile_stage(GL_FRAGMENT_SHADER, with_defines(*fsrc, defines_), ferr);
    if (vs == 0 || fs == 0) {
        last_error_ = "shader '" + name + "' compile failed: " + verr + ferr;
        IMMUNE_LOG_ERROR("%s", last_error_.c_str());
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        entries_.push_back(e);
        return ShaderProgram{};
    }
    const GLuint prog = link_program(vs, fs, lerr);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (prog == 0) {
        last_error_ = "shader '" + name + "' link failed: " + lerr;
        IMMUNE_LOG_ERROR("%s", last_error_.c_str());
        entries_.push_back(e);
        return ShaderProgram{};
    }
    e->program = ShaderProgram{prog};
    entries_.push_back(e);
    return e->program;
}

ShaderProgram ShaderManager::load_compute(const std::string& name, const std::string& comp_path) {
    auto* e = new Entry{};
    e->name = name;
    e->is_compute = true;
    e->comp_path = comp_path;

    const auto csrc = platform::read_text_file(shader_path(comp_path));
    if (!csrc) {
        last_error_ = "shader '" + name + "': failed to read " + comp_path;
        IMMUNE_LOG_ERROR("%s", last_error_.c_str());
        entries_.push_back(e);
        return ShaderProgram{};
    }
    e->last_comp_src = *csrc;

    std::string cerr, lerr;
    const GLuint cs = compile_stage(GL_COMPUTE_SHADER, with_defines(*csrc, defines_), cerr);
    if (cs == 0) {
        last_error_ = "shader '" + name + "' compile failed: " + cerr;
        IMMUNE_LOG_ERROR("%s", last_error_.c_str());
        entries_.push_back(e);
        return ShaderProgram{};
    }
    const GLuint prog = link_program(cs, 0, lerr);
    glDeleteShader(cs);
    if (prog == 0) {
        last_error_ = "shader '" + name + "' link failed: " + lerr;
        IMMUNE_LOG_ERROR("%s", last_error_.c_str());
        entries_.push_back(e);
        return ShaderProgram{};
    }
    e->program = ShaderProgram{prog};
    entries_.push_back(e);
    return e->program;
}

ShaderProgram ShaderManager::get(const std::string& name) const {
    for (const Entry* e : entries_) {
        if (e != nullptr && e->name == name) return e->program;
    }
    return ShaderProgram{};
}

u32 ShaderManager::poll_reload() {
    u32 reloaded = 0;
    for (Entry* e : entries_) {
        if (e == nullptr) continue;
        if (e->is_compute) {
            const auto csrc = platform::read_text_file(shader_path(e->comp_path));
            if (!csrc || *csrc == e->last_comp_src) continue;
            std::string cerr, lerr;
            const GLuint cs = compile_stage(GL_COMPUTE_SHADER, with_defines(*csrc, defines_), cerr);
            if (cs == 0) {
                last_error_ = "shader '" + e->name + "' reload compile failed: " + cerr;
                IMMUNE_LOG_ERROR("%s", last_error_.c_str());
                e->last_comp_src = *csrc; // don't retry an unchanged bad file every poll
                continue;
            }
            const GLuint prog = link_program(cs, 0, lerr);
            glDeleteShader(cs);
            if (prog == 0) {
                last_error_ = "shader '" + e->name + "' reload link failed: " + lerr;
                IMMUNE_LOG_ERROR("%s", last_error_.c_str());
                e->last_comp_src = *csrc;
                continue;
            }
            if (e->program.gl_id != 0) glDeleteProgram(e->program.gl_id);
            e->program = ShaderProgram{prog};
            e->last_comp_src = *csrc;
            ++reloaded;
            continue;
        }

        const auto vsrc = platform::read_text_file(shader_path(e->vert_path));
        const auto fsrc = platform::read_text_file(shader_path(e->frag_path));
        if (!vsrc || !fsrc) continue;
        if (*vsrc == e->last_vert_src && *fsrc == e->last_frag_src) continue;

        std::string verr, ferr, lerr;
        const GLuint vs = compile_stage(GL_VERTEX_SHADER, with_defines(*vsrc, defines_), verr);
        const GLuint fs = compile_stage(GL_FRAGMENT_SHADER, with_defines(*fsrc, defines_), ferr);
        if (vs == 0 || fs == 0) {
            last_error_ = "shader '" + e->name + "' reload compile failed: " + verr + ferr;
            IMMUNE_LOG_ERROR("%s", last_error_.c_str());
            if (vs != 0) glDeleteShader(vs);
            if (fs != 0) glDeleteShader(fs);
            // Keep the previous working program; remember this source so an
            // unchanged-but-broken file doesn't retry every poll.
            e->last_vert_src = *vsrc;
            e->last_frag_src = *fsrc;
            continue;
        }
        const GLuint prog = link_program(vs, fs, lerr);
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (prog == 0) {
            last_error_ = "shader '" + e->name + "' reload link failed: " + lerr;
            IMMUNE_LOG_ERROR("%s", last_error_.c_str());
            e->last_vert_src = *vsrc;
            e->last_frag_src = *fsrc;
            continue;
        }
        if (e->program.gl_id != 0) glDeleteProgram(e->program.gl_id);
        e->program = ShaderProgram{prog};
        e->last_vert_src = *vsrc;
        e->last_frag_src = *fsrc;
        ++reloaded;
    }
    return reloaded;
}

void ShaderManager::set_define(const std::string& key, const std::string& value) {
    for (auto& d : defines_) {
        if (d.first == key) { d.second = value; return; }
    }
    defines_.emplace_back(key, value);
}

void ShaderManager::clear() {
    for (Entry* e : entries_) {
        if (e != nullptr && e->program.gl_id != 0) glDeleteProgram(e->program.gl_id);
        delete e;
    }
    entries_.clear();
}

} // namespace immune::render
