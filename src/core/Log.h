// core/Log.h — minimal leveled logger. Never called from the hot path.
#pragma once

#include "core/Types.h"

#include <string>
#include <string_view>

namespace immune::log {

enum class Level : u8 { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

/// Sets the minimum level that will be emitted. Default: Info.
void set_level(Level level);
Level level();

/// Routes output to stderr (default) or suppresses it entirely.
/// --bench and --sim-test set this so stdout stays pure machine-readable JSON.
void set_enabled(bool enabled);

void write(Level level, std::string_view message);

/// printf-style helpers. Format is checked at runtime only; these are not hot-path.
void writef(Level level, const char* fmt, ...);

} // namespace immune::log

#define IMMUNE_LOG_TRACE(...) ::immune::log::writef(::immune::log::Level::Trace, __VA_ARGS__)
#define IMMUNE_LOG_DEBUG(...) ::immune::log::writef(::immune::log::Level::Debug, __VA_ARGS__)
#define IMMUNE_LOG_INFO(...)  ::immune::log::writef(::immune::log::Level::Info,  __VA_ARGS__)
#define IMMUNE_LOG_WARN(...)  ::immune::log::writef(::immune::log::Level::Warn,  __VA_ARGS__)
#define IMMUNE_LOG_ERROR(...) ::immune::log::writef(::immune::log::Level::Error, __VA_ARGS__)
