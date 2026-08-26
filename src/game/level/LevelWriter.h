// game/level/LevelWriter.h — LevelDef -> JSON. NEW MODULE.
//
// RATIONALE
// Level.cpp has had a parser and no serializer since the schema was written,
// which is why nothing in this project can round-trip a level: not a script
// generating content, not `--level-fmt`, and not an editor that has to write
// back what you dragged. This file is the missing half, and every later phase
// of the level editor sits on it.
//
// CANONICAL, NOT MERELY VALID
// A level file is a git artifact before it is a game asset, so the output is
// pinned in four ways that plain nlohmann::dump() does not give:
//
//   1. FIXED KEY ORDER, matching authoring order rather than nlohmann's default
//      map ordering -- which alphabetises, and therefore strands `waves` in the
//      middle of the geometry.
//   2. DEFAULTS OMITTED. The parser is `j.value(key, default)` throughout, so
//      writing a field that equals its default is pure noise; a hand-authored
//      level that never mentioned `lane_id` must not grow one on save, or every
//      diff becomes unreadable. See the identity exception below.
//   3. THREE-DECIMAL FLOATS, trailing zeros trimmed. The shipped content
//      already looks like this (287.378); making it a rule is what stops a drag
//      gizmo emitting 287.37799072265625 and a one-pixel nudge rewriting a file.
//   4. LAYOUT BY EDIT UNIT. One line per vessel control point, per obstacle,
//      per spawn entry, per zone, with vectors inline -- because a diff should
//      show one changed line when you drag one thing.
//
// THE IDENTITY EXCEPTION
// `schema`, `waves`, a spawn entry's `family` and `count`, and a vessel point's
// `w` are always written even when they equal their defaults. `{"duration":4}`
// is a legal spawn entry meaning zero viruses and a point with no width is
// meaningless to a reader; no author should have to know the defaults to tell
// what a record IS. The rule is: omit defaults, except for the fields that say
// what the record is.
//
// ROUND-TRIP
// parse -> write -> parse reproduces the LevelDef to within the 3-decimal
// rounding (level_equal's tolerance), and write is IDEMPOTENT: writing an
// already-canonical level returns byte-identical text. tests/test_level_writer
// asserts both across every file in assets/levels.
#pragma once

#include "game/level/Level.h"

#include <string>

namespace immune::game {

/// Canonical JSON for `def`, per the rules above. Always ends with a newline.
///
/// Pure: touches no files and no sim. A LevelDef that would fail validate() is
/// still written faithfully -- refusing to save broken work in progress is a
/// policy decision for the caller, not this function's.
std::string level_to_json(const LevelDef& def);

/// level_to_json() written to `path`, creating parent directories if needed.
/// False with `err` set on any I/O failure; never throws.
///
/// Deliberately does NOT back up, prompt, or refuse to overwrite -- the editor's
/// save flow owns those, and a headless caller (--level-fmt) must not be made
/// to opt out of them.
bool level_save_file(const LevelDef& def, const std::string& path, std::string& err);

/// Field-by-field equality, floats compared to `eps`.
///
/// The default tolerance is the writer's own rounding quantum: canonical output
/// carries three decimals, so two LevelDefs that serialize identically are
/// equal by this test and nothing finer is observable in a file. Backs the
/// round-trip test and the editor's dirty flag.
bool level_equal(const LevelDef& a, const LevelDef& b, f32 eps = 1e-3f);

} // namespace immune::game
