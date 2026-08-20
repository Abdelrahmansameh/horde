// config/Field.cpp — generic get/set over a Schema's field list.
#include "config/Field.h"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace immune::config {
namespace {

/// Shortest round-trippable text for a number. std::to_chars is used rather
/// than printf so that dump -> load -> dump is byte-stable.
template <class T>
std::string number_to_string(T value) {
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    if (res.ec != std::errc{}) return "0";
    return std::string(buf, res.ptr);
}

template <class T>
bool string_to_number(std::string_view text, T& out) {
    // A leading '+' is natural to a human but rejected by from_chars.
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto res = std::from_chars(begin, end, out);
    return res.ec == std::errc{} && res.ptr == end;
}

/// Reads a comma-separated list of floats, e.g. "0.2,0.8,0.9,0.5".
bool string_to_floats(std::string_view text, f32* out, usize count) {
    usize written = 0;
    usize start = 0;
    while (written < count) {
        const usize comma = text.find(',', start);
        const usize len = comma == std::string_view::npos ? std::string_view::npos : comma - start;
        std::string_view piece = text.substr(start, len);
        while (!piece.empty() && piece.front() == ' ') piece.remove_prefix(1);
        while (!piece.empty() && piece.back() == ' ') piece.remove_suffix(1);
        if (!string_to_number(piece, out[written])) return false;
        ++written;
        if (comma == std::string_view::npos) return written == count;
        start = comma + 1;
    }
    // More components than the field holds.
    return false;
}

template <class T>
T* at(void* base, usize offset) {
    return reinterpret_cast<T*>(static_cast<u8*>(base) + offset);
}

template <class T>
const T* at(const void* base, usize offset) {
    return reinterpret_cast<const T*>(static_cast<const u8*>(base) + offset);
}

i64 read_enum(const Field& field, const void* base) {
    return field.kind == FieldKind::EnumU8
               ? static_cast<i64>(*at<u8>(base, field.offset))
               : static_cast<i64>(*at<u32>(base, field.offset));
}

void write_enum(const Field& field, void* base, i64 value) {
    if (field.kind == FieldKind::EnumU8) {
        *at<u8>(base, field.offset) = static_cast<u8>(value);
    } else {
        *at<u32>(base, field.offset) = static_cast<u32>(value);
    }
}

std::string accepted_enum_names(const Field& field) {
    std::string accepted;
    for (const EnumEntry& e : enum_entries(field)) {
        if (!accepted.empty()) accepted += ", ";
        accepted += e.name;
    }
    return accepted;
}

} // namespace

const Field* Schema::find(std::string_view field_name) const {
    const auto it = std::find_if(fields.begin(), fields.end(),
                                 [&](const Field& f) { return field_name == f.name; });
    return it == fields.end() ? nullptr : &*it;
}

std::span<const EnumEntry> enum_entries(const Field& field) {
    if (field.enum_values == nullptr) return {};
    usize count = 0;
    while (field.enum_values[count].name != nullptr) ++count;
    return {field.enum_values, count};
}

std::string_view enum_name(const Field& field, const void* base) {
    const i64 stored = read_enum(field, base);
    for (const EnumEntry& e : enum_entries(field)) {
        if (e.value == stored) return e.name;
    }
    return {};
}

bool field_to_string(const Schema& schema, const void* base, std::string_view field_name,
                     std::string& out) {
    const Field* field = schema.find(field_name);
    if (field == nullptr) return false;
    switch (field->kind) {
        case FieldKind::F32: out = number_to_string(*at<f32>(base, field->offset)); return true;
        case FieldKind::F64: out = number_to_string(*at<f64>(base, field->offset)); return true;
        case FieldKind::U32: out = number_to_string(*at<u32>(base, field->offset)); return true;
        case FieldKind::U64: out = number_to_string(*at<u64>(base, field->offset)); return true;
        case FieldKind::I32: out = number_to_string(*at<i32>(base, field->offset)); return true;
        case FieldKind::U8:  out = number_to_string(*at<u8>(base, field->offset));  return true;
        case FieldKind::Bool:
            out = *at<bool>(base, field->offset) ? "true" : "false";
            return true;
        case FieldKind::Vec2: {
            const Vec2& v = *at<Vec2>(base, field->offset);
            out = number_to_string(v.x) + "," + number_to_string(v.y);
            return true;
        }
        case FieldKind::Vec4: {
            const Vec4& v = *at<Vec4>(base, field->offset);
            out = number_to_string(v.x) + "," + number_to_string(v.y) + "," +
                  number_to_string(v.z) + "," + number_to_string(v.w);
            return true;
        }
        case FieldKind::String: out = *at<std::string>(base, field->offset); return true;
        case FieldKind::EnumU8:
        case FieldKind::EnumU32: out = std::string(enum_name(*field, base)); return true;
    }
    return false;
}

bool field_from_string(const Schema& schema, void* base, std::string_view field_name,
                       std::string_view value, std::string& err) {
    const Field* field = schema.find(field_name);
    if (field == nullptr) {
        err = "unknown field ";
        err += quote(field_name);
        return false;
    }
    const auto bad = [&](const char* expected) {
        err = "field " + quote(field_name) + " expects " + expected + ", got " + quote(value);
        return false;
    };
    switch (field->kind) {
        case FieldKind::F32:
            return string_to_number(value, *at<f32>(base, field->offset)) || bad("a number");
        case FieldKind::F64:
            return string_to_number(value, *at<f64>(base, field->offset)) || bad("a number");
        case FieldKind::U32:
            return string_to_number(value, *at<u32>(base, field->offset)) ||
                   bad("a non-negative integer");
        case FieldKind::U64:
            return string_to_number(value, *at<u64>(base, field->offset)) ||
                   bad("a non-negative integer");
        case FieldKind::I32:
            return string_to_number(value, *at<i32>(base, field->offset)) || bad("an integer");
        case FieldKind::U8: {
            u32 wide = 0;
            if (!string_to_number(value, wide) || wide > 255u) return bad("an integer 0..255");
            *at<u8>(base, field->offset) = static_cast<u8>(wide);
            return true;
        }
        case FieldKind::Bool:
            if (value == "true" || value == "1") {
                *at<bool>(base, field->offset) = true;
                return true;
            }
            if (value == "false" || value == "0") {
                *at<bool>(base, field->offset) = false;
                return true;
            }
            return bad("true or false");
        case FieldKind::Vec2: {
            f32 v[2]{};
            if (!string_to_floats(value, v, 2)) return bad("two comma-separated numbers");
            *at<Vec2>(base, field->offset) = Vec2{v[0], v[1]};
            return true;
        }
        case FieldKind::Vec4: {
            f32 v[4]{};
            if (!string_to_floats(value, v, 4)) return bad("four comma-separated numbers");
            *at<Vec4>(base, field->offset) = Vec4{v[0], v[1], v[2], v[3]};
            return true;
        }
        case FieldKind::String:
            *at<std::string>(base, field->offset) = std::string(value);
            return true;
        case FieldKind::EnumU8:
        case FieldKind::EnumU32:
            for (const EnumEntry& e : enum_entries(*field)) {
                if (value == e.name) {
                    write_enum(*field, base, e.value);
                    return true;
                }
            }
            err = "field " + quote(field_name) + ": unknown value " + quote(value) +
                  " (accepted: " + accepted_enum_names(*field) + ")";
            return false;
    }
    return false;
}

std::string quote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out += '\'';
    out += text;
    out += '\'';
    return out;
}

} // namespace immune::config
