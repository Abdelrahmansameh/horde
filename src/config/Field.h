// config/Field.h — the field descriptor that makes one declaration do four jobs.
//
// RATIONALE
//  - Every config field is required, and the config must also support
//    `config get <path>`, `config set <path> <value>` from the gym console, and
//    a `config dump` that emits a complete file. Hand-writing parsers the way
//    game/level/Level.cpp does would mean writing each field's name four times
//    in four places that then drift apart.
//  - So a struct declares its fields ONCE as a Schema, and parse / dump /
//    get-by-name / set-by-name are all derived from that list. A dumped file is
//    therefore guaranteed to round-trip, and a field that exists is guaranteed
//    to be addressable from the console.
//  - The cost is offsetof-based access, which is why every config struct must
//    be a standard-layout aggregate of scalars. IMMUNE_CONFIG_SCHEMA asserts it.
#pragma once

#include "core/Types.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace immune::config {

enum class FieldKind : u8 {
    F32,
    F64,
    U32,
    U64,
    I32,
    U8,
    Bool,
    Vec2,
    Vec4,
    String,   ///< storage is std::string
    EnumU8,   ///< storage is an enum class with u8 underlying type
    EnumU32,  ///< storage is an enum class with u32/i32 underlying type
};

/// One accepted spelling of an enum-valued field. Tables are terminated by a
/// {nullptr, 0} sentinel so they can live as plain static arrays.
struct EnumEntry {
    const char* name;
    i64 value;
};

struct Field {
    const char* name;
    FieldKind kind;
    usize offset;
    const char* doc = "";
    /// Required for EnumU8/EnumU32, ignored otherwise. Sentinel-terminated.
    const EnumEntry* enum_values = nullptr;
};

/// A named list of fields belonging to one struct type.
struct Schema {
    const char* name;
    std::span<const Field> fields;

    const Field* find(std::string_view field_name) const;
};

/// Declares a field. `Struct` is the owning type, `member` the member name;
/// the JSON key is the member name verbatim, so the file reads like the code.
#define IMMUNE_CONFIG_FIELD(Struct, member, kind, doc) \
    ::immune::config::Field{#member, kind, offsetof(Struct, member), doc}

/// As above, for an enum-valued member. `values` is a sentinel-terminated
/// static array of EnumEntry.
#define IMMUNE_CONFIG_ENUM_FIELD(Struct, member, kind, doc, values) \
    ::immune::config::Field{#member, kind, offsetof(Struct, member), doc, values}

/// Guards the offsetof precondition at the point the schema is declared.
#define IMMUNE_CONFIG_SCHEMA_ASSERT(Struct) \
    static_assert(std::is_standard_layout_v<Struct>, \
                  #Struct " must be standard-layout to be addressed by offsetof")

// --- Generic access. `base` points at an instance of the schema's struct. ---

/// Reads `field` out of `base` and formats it as the same text `config set`
/// accepts. False if the field name is unknown.
bool field_to_string(const Schema& schema, const void* base, std::string_view field,
                     std::string& out);

/// Parses `value` and writes it into `base`. False (with `err` set) on an
/// unknown field name or unparseable text.
bool field_from_string(const Schema& schema, void* base, std::string_view field,
                       std::string_view value, std::string& err);

/// Name of the enum value currently stored in an enum field, or "" if the
/// stored value matches no entry.
std::string_view enum_name(const Field& field, const void* base);

std::span<const EnumEntry> enum_entries(const Field& field);

/// Wraps text in single quotes for error messages. Config errors quote every
/// key and value they mention so a stray space or newline is visible.
std::string quote(std::string_view text);

} // namespace immune::config
