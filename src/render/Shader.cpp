// Wave 0 stub. Implementation is owned by Wave 1C.
#include "render/Shader.h"

namespace immune::render {

struct ShaderManager::Entry {
    std::string name;
    ShaderProgram program;
};

ShaderManager::~ShaderManager() { clear(); }

ShaderProgram ShaderManager::load_graphics(const std::string&, const std::string&, const std::string&) {
    return ShaderProgram{};
}

ShaderProgram ShaderManager::load_compute(const std::string&, const std::string&) {
    return ShaderProgram{};
}

ShaderProgram ShaderManager::get(const std::string& name) const {
    for (const Entry* e : entries_) {
        if (e && e->name == name) return e->program;
    }
    return ShaderProgram{};
}

u32 ShaderManager::poll_reload() { return 0; }

void ShaderManager::set_define(const std::string& key, const std::string& value) {
    for (auto& d : defines_) {
        if (d.first == key) { d.second = value; return; }
    }
    defines_.emplace_back(key, value);
}

void ShaderManager::clear() {
    for (Entry* e : entries_) delete e;
    entries_.clear();
}

} // namespace immune::render
