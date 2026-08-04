#include "core/Log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace immune::log {
namespace {
Level g_level = Level::Info;
bool g_enabled = true;
std::mutex g_mutex;

const char* level_tag(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
        default:           return "?????";
    }
}
} // namespace

void set_level(Level l) { g_level = l; }
Level level() { return g_level; }
void set_enabled(bool enabled) { g_enabled = enabled; }

void write(Level l, std::string_view message) {
    if (!g_enabled || l < g_level) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "[%s] %.*s\n", level_tag(l),
                 static_cast<int>(message.size()), message.data());
}

void writef(Level l, const char* fmt, ...) {
    if (!g_enabled || l < g_level) return;
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    write(l, buffer);
}

} // namespace immune::log
