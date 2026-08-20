// config/Json.h — strict JSON reading helpers for the tuning config.
//
// RATIONALE
//  - The config files are AUTHORITATIVE: a missing field is an error, not a
//    silent fallback to a compiled-in default. That is what makes a shipped
//    config file a complete, self-documenting list of every knob that exists.
//  - An unknown key is also an error. A typo'd key would otherwise be a tweak
//    that silently does nothing, which is the worst possible failure mode for
//    a balance file.
//  - nlohmann's own exceptions name a byte offset, which is useless to whoever
//    is editing the file. Every helper here throws std::runtime_error carrying
//    a field path ("towers.macrophage[2].stats: missing required field
//    'damage'"). ConfigStore::load_dir() is the single place that catches.
#pragma once

#include "config/Field.h"
#include "core/Types.h"

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace immune::config {

using Json = nlohmann::json;

/// Scope stack that turns nested parsing into a readable field path.
/// Push through Ctx::Scope so the stack unwinds correctly when a helper throws.
class Ctx {
public:
    explicit Ctx(std::string root);

    void push(std::string_view name);
    void push_index(usize index);
    void pop();

    /// e.g. "towers.json: towers.macrophage[2].stats"
    std::string path() const;

    [[noreturn]] void fail(std::string_view message) const;

    /// RAII scope guard. Prefer this over bare push/pop.
    class Scope {
    public:
        Scope(Ctx& ctx, std::string_view name) : ctx_(ctx) { ctx_.push(name); }
        Scope(Ctx& ctx, usize index) : ctx_(ctx) { ctx_.push_index(index); }
        ~Scope() { ctx_.pop(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        Ctx& ctx_;
    };

private:
    std::string root_;
    std::vector<std::string> parts_;
};

// --- Required-field readers. Each throws with the Ctx path on absence or on a
// --- type mismatch; there is deliberately no defaulting overload.
f32         require_f32   (const Json& j, const char* key, const Ctx& ctx);
f64         require_f64   (const Json& j, const char* key, const Ctx& ctx);
u32         require_u32   (const Json& j, const char* key, const Ctx& ctx);
u64         require_u64   (const Json& j, const char* key, const Ctx& ctx);
i32         require_i32   (const Json& j, const char* key, const Ctx& ctx);
u8          require_u8    (const Json& j, const char* key, const Ctx& ctx);
bool        require_bool  (const Json& j, const char* key, const Ctx& ctx);
Vec2        require_vec2  (const Json& j, const char* key, const Ctx& ctx);
Vec4        require_vec4  (const Json& j, const char* key, const Ctx& ctx);
std::string require_string(const Json& j, const char* key, const Ctx& ctx);

const Json& require_object(const Json& j, const char* key, const Ctx& ctx);
const Json& require_array (const Json& j, const char* key, const Ctx& ctx);

/// Reads a required string key and maps it through `values`. Unknown names
/// fail with the full list of accepted names, so the error is actionable.
i64 require_enum(const Json& j, const char* key, std::span<const EnumEntry> values,
                 const Ctx& ctx);

/// Errors if `j` carries any key not named in `allowed`. Suggests the closest
/// allowed name when the unknown key looks like a typo.
void reject_unknown_keys(const Json& j, std::span<const std::string_view> allowed,
                         const Ctx& ctx);
/// Overload for a schema's field list — the common case.
void reject_unknown_keys(const Json& j, const Schema& schema, const Ctx& ctx);

/// Closest match to `key` among `candidates` within a small edit distance, or
/// empty if nothing is close enough. Used only to decorate error messages.
std::string_view closest_name(std::string_view key, std::span<const std::string_view> candidates);

// --- Schema-driven struct IO. These are the two halves that guarantee a
// --- dumped config file loads back to identical values.

/// Reads every field named by `schema` out of `j` into `base`. Missing fields
/// and unknown keys both throw. `j` must be an object.
void parse_struct(const Json& j, const Schema& schema, void* base, Ctx& ctx);

/// Writes every field named by `schema` into `j`, in declaration order.
void dump_struct(Json& j, const Schema& schema, const void* base);

} // namespace immune::config
