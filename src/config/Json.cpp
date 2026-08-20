// config/Json.cpp — strict readers and schema-driven struct IO.
#include "config/Json.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <system_error>

namespace immune::config {
namespace {

/// Field paths read better with array indices attached to the preceding name
/// ("spawns[2]") than separated by a dot, so index parts carry their own
/// bracket and are joined without a separator.
bool is_index_part(std::string_view part) {
    return !part.empty() && part.front() == '[';
}

[[noreturn]] void throw_at(const Ctx& ctx, std::string_view message) {
    throw std::runtime_error(ctx.path() + ": " + std::string(message));
}

/// Levenshtein distance, capped: we only care whether it is small.
usize edit_distance(std::string_view a, std::string_view b) {
    std::vector<usize> prev(b.size() + 1);
    std::vector<usize> curr(b.size() + 1);
    for (usize j = 0; j <= b.size(); ++j) prev[j] = j;
    for (usize i = 1; i <= a.size(); ++i) {
        curr[0] = i;
        for (usize j = 1; j <= b.size(); ++j) {
            const usize cost = a[i - 1] == b[j - 1] ? 0u : 1u;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = curr;
    }
    return prev[b.size()];
}

const Json& require_member(const Json& j, const char* key, const Ctx& ctx) {
    if (!j.is_object()) throw_at(ctx, "expected an object");
    const auto it = j.find(key);
    if (it == j.end()) {
        throw_at(ctx, "missing required field " + quote(key));
    }
    return *it;
}

[[noreturn]] void wrong_type(const Ctx& ctx, const char* key, const char* expected,
                             const Json& value) {
    throw_at(ctx, "field " + quote(key) + " must be " + expected + ", got " +
                      std::string(value.type_name()));
}

/// Shared numeric extraction: JSON has one number type, so an integer field
/// accepts 3 but must reject 3.5 rather than silently truncating a tweak.
f64 require_number(const Json& j, const char* key, const Ctx& ctx) {
    const Json& v = require_member(j, key, ctx);
    if (!v.is_number()) wrong_type(ctx, key, "a number", v);
    return v.get<f64>();
}

f64 require_integral(const Json& j, const char* key, const Ctx& ctx, f64 min, f64 max) {
    const f64 raw = require_number(j, key, ctx);
    if (!(raw >= min && raw <= max)) {  // also catches NaN
        throw_at(ctx, "field " + quote(key) + " is out of range");
    }
    if (static_cast<f64>(static_cast<i64>(raw)) != raw) {
        throw_at(ctx, "field " + quote(key) + " must be a whole number");
    }
    return raw;
}

void require_float_array(const Json& j, const char* key, const Ctx& ctx, usize count,
                         f32* out) {
    const Json& v = require_member(j, key, ctx);
    if (!v.is_array() || v.size() != count) {
        throw_at(ctx, "field " + quote(key) + " must be an array of " +
                          std::to_string(count) + " numbers");
    }
    for (usize i = 0; i < count; ++i) {
        if (!v.at(i).is_number()) {
            throw_at(ctx, "field " + quote(key) + "[" + std::to_string(i) + "] must be a number");
        }
        out[i] = v.at(i).get<f32>();
    }
}

/// JSON numbers are doubles, so writing an f32 straight through prints the
/// widening artefact: 0.7f becomes 0.699999988079071. These files are meant to
/// be edited by hand, so an f32 is emitted as the SHORTEST decimal that reads
/// back as the same f32 — 0.7 stays 0.7, and the round-trip is still exact.
f64 f32_as_json_number(f32 value) {
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    if (res.ec != std::errc{}) return static_cast<f64>(value);
    f64 widened = 0.0;
    const auto back = std::from_chars(buf, res.ptr, widened);
    return back.ec == std::errc{} ? widened : static_cast<f64>(value);
}

} // namespace

// --- Ctx ---------------------------------------------------------------

Ctx::Ctx(std::string root) : root_(std::move(root)) {}

void Ctx::push(std::string_view name) { parts_.emplace_back(name); }

void Ctx::push_index(usize index) { parts_.push_back("[" + std::to_string(index) + "]"); }

void Ctx::pop() {
    if (!parts_.empty()) parts_.pop_back();
}

std::string Ctx::path() const {
    std::string out = root_;
    bool wrote_part = false;
    for (const std::string& part : parts_) {
        if (!wrote_part) {
            if (!out.empty()) out += ": ";
        } else if (!is_index_part(part)) {
            out += '.';
        }
        out += part;
        wrote_part = true;
    }
    return out;
}

void Ctx::fail(std::string_view message) const { throw_at(*this, message); }

// --- Required readers --------------------------------------------------

f32 require_f32(const Json& j, const char* key, const Ctx& ctx) {
    return static_cast<f32>(require_number(j, key, ctx));
}

f64 require_f64(const Json& j, const char* key, const Ctx& ctx) {
    return require_number(j, key, ctx);
}

u32 require_u32(const Json& j, const char* key, const Ctx& ctx) {
    return static_cast<u32>(require_integral(j, key, ctx, 0.0, 4294967295.0));
}

u64 require_u64(const Json& j, const char* key, const Ctx& ctx) {
    const Json& v = require_member(j, key, ctx);
    if (!v.is_number_unsigned() && !v.is_number_integer()) {
        wrong_type(ctx, key, "a non-negative integer", v);
    }
    if (v.is_number_integer() && !v.is_number_unsigned() && v.get<i64>() < 0) {
        throw_at(ctx, "field " + quote(key) + " must be non-negative");
    }
    return v.get<u64>();
}

i32 require_i32(const Json& j, const char* key, const Ctx& ctx) {
    return static_cast<i32>(require_integral(j, key, ctx, -2147483648.0, 2147483647.0));
}

u8 require_u8(const Json& j, const char* key, const Ctx& ctx) {
    return static_cast<u8>(require_integral(j, key, ctx, 0.0, 255.0));
}

bool require_bool(const Json& j, const char* key, const Ctx& ctx) {
    const Json& v = require_member(j, key, ctx);
    if (!v.is_boolean()) wrong_type(ctx, key, "true or false", v);
    return v.get<bool>();
}

Vec2 require_vec2(const Json& j, const char* key, const Ctx& ctx) {
    f32 v[2]{};
    require_float_array(j, key, ctx, 2, v);
    return Vec2{v[0], v[1]};
}

Vec4 require_vec4(const Json& j, const char* key, const Ctx& ctx) {
    f32 v[4]{};
    require_float_array(j, key, ctx, 4, v);
    return Vec4{v[0], v[1], v[2], v[3]};
}

std::string require_string(const Json& j, const char* key, const Ctx& ctx) {
    const Json& v = require_member(j, key, ctx);
    if (!v.is_string()) wrong_type(ctx, key, "a string", v);
    return v.get<std::string>();
}

const Json& require_object(const Json& j, const char* key, const Ctx& ctx) {
    const Json& v = require_member(j, key, ctx);
    if (!v.is_object()) wrong_type(ctx, key, "an object", v);
    return v;
}

const Json& require_array(const Json& j, const char* key, const Ctx& ctx) {
    const Json& v = require_member(j, key, ctx);
    if (!v.is_array()) wrong_type(ctx, key, "an array", v);
    return v;
}

i64 require_enum(const Json& j, const char* key, std::span<const EnumEntry> values,
                 const Ctx& ctx) {
    const std::string name = require_string(j, key, ctx);
    std::string accepted;
    for (const EnumEntry& e : values) {
        if (name == e.name) return e.value;
        if (!accepted.empty()) accepted += ", ";
        accepted += e.name;
    }
    throw_at(ctx, "field " + quote(key) + ": unknown value " + quote(name) +
                      " (accepted: " + accepted + ")");
}

// --- Unknown-key rejection ---------------------------------------------

std::string_view closest_name(std::string_view key,
                              std::span<const std::string_view> candidates) {
    // A third of the key length, so short names need a near-exact match and
    // long ones tolerate a typo or two.
    const usize limit = std::max<usize>(1, key.size() / 3);
    std::string_view best;
    usize best_distance = limit + 1;
    for (std::string_view candidate : candidates) {
        const usize d = edit_distance(key, candidate);
        if (d < best_distance) {
            best_distance = d;
            best = candidate;
        }
    }
    return best_distance <= limit ? best : std::string_view{};
}

void reject_unknown_keys(const Json& j, std::span<const std::string_view> allowed,
                         const Ctx& ctx) {
    if (!j.is_object()) return;
    for (const auto& entry : j.items()) {
        const std::string& key = entry.key();
        if (std::find(allowed.begin(), allowed.end(), key) != allowed.end()) continue;
        const std::string_view suggestion = closest_name(key, allowed);
        std::string message = "unknown field " + quote(key);
        if (!suggestion.empty()) {
            message += " (did you mean " + quote(suggestion) + "?)";
        }
        throw_at(ctx, message);
    }
}

void reject_unknown_keys(const Json& j, const Schema& schema, const Ctx& ctx) {
    std::vector<std::string_view> names;
    names.reserve(schema.fields.size());
    for (const Field& f : schema.fields) names.emplace_back(f.name);
    reject_unknown_keys(j, names, ctx);
}

// --- Schema-driven struct IO -------------------------------------------

void parse_struct(const Json& j, const Schema& schema, void* base, Ctx& ctx) {
    if (!j.is_object()) throw_at(ctx, "expected an object");
    reject_unknown_keys(j, schema, ctx);
    for (const Field& field : schema.fields) {
        auto* bytes = static_cast<u8*>(base) + field.offset;
        switch (field.kind) {
            case FieldKind::F32:  *reinterpret_cast<f32*>(bytes) = require_f32(j, field.name, ctx); break;
            case FieldKind::F64:  *reinterpret_cast<f64*>(bytes) = require_f64(j, field.name, ctx); break;
            case FieldKind::U32:  *reinterpret_cast<u32*>(bytes) = require_u32(j, field.name, ctx); break;
            case FieldKind::U64:  *reinterpret_cast<u64*>(bytes) = require_u64(j, field.name, ctx); break;
            case FieldKind::I32:  *reinterpret_cast<i32*>(bytes) = require_i32(j, field.name, ctx); break;
            case FieldKind::U8:   *reinterpret_cast<u8*>(bytes)  = require_u8(j, field.name, ctx);  break;
            case FieldKind::Bool: *reinterpret_cast<bool*>(bytes) = require_bool(j, field.name, ctx); break;
            case FieldKind::Vec2: *reinterpret_cast<Vec2*>(bytes) = require_vec2(j, field.name, ctx); break;
            case FieldKind::Vec4: *reinterpret_cast<Vec4*>(bytes) = require_vec4(j, field.name, ctx); break;
            case FieldKind::String:
                *reinterpret_cast<std::string*>(bytes) = require_string(j, field.name, ctx);
                break;
            case FieldKind::EnumU8:
                *reinterpret_cast<u8*>(bytes) =
                    static_cast<u8>(require_enum(j, field.name, enum_entries(field), ctx));
                break;
            case FieldKind::EnumU32:
                *reinterpret_cast<u32*>(bytes) =
                    static_cast<u32>(require_enum(j, field.name, enum_entries(field), ctx));
                break;
        }
    }
}

void dump_struct(Json& j, const Schema& schema, const void* base) {
    for (const Field& field : schema.fields) {
        const auto* bytes = static_cast<const u8*>(base) + field.offset;
        switch (field.kind) {
            case FieldKind::F32:
                j[field.name] = f32_as_json_number(*reinterpret_cast<const f32*>(bytes));
                break;
            case FieldKind::F64:  j[field.name] = *reinterpret_cast<const f64*>(bytes); break;
            case FieldKind::U32:  j[field.name] = *reinterpret_cast<const u32*>(bytes); break;
            case FieldKind::U64:  j[field.name] = *reinterpret_cast<const u64*>(bytes); break;
            case FieldKind::I32:  j[field.name] = *reinterpret_cast<const i32*>(bytes); break;
            case FieldKind::U8:   j[field.name] = *reinterpret_cast<const u8*>(bytes);  break;
            case FieldKind::Bool: j[field.name] = *reinterpret_cast<const bool*>(bytes); break;
            case FieldKind::Vec2: {
                const Vec2& v = *reinterpret_cast<const Vec2*>(bytes);
                j[field.name] = Json::array({f32_as_json_number(v.x), f32_as_json_number(v.y)});
                break;
            }
            case FieldKind::Vec4: {
                const Vec4& v = *reinterpret_cast<const Vec4*>(bytes);
                j[field.name] =
                    Json::array({f32_as_json_number(v.x), f32_as_json_number(v.y),
                                 f32_as_json_number(v.z), f32_as_json_number(v.w)});
                break;
            }
            case FieldKind::String:
                j[field.name] = *reinterpret_cast<const std::string*>(bytes);
                break;
            case FieldKind::EnumU8:
            case FieldKind::EnumU32:
                j[field.name] = std::string(enum_name(field, base));
                break;
        }
    }
}

} // namespace immune::config
