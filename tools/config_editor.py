#!/usr/bin/env python3
"""tools/config_editor.py — a browser UI for tweaking assets/config/*.json.

Serves a single page on localhost that renders every tuning file as sliders,
toggles, colour pickers and enum dropdowns, and writes the file back on Save
(or on every change, with auto-save on). The game polls the config directory
by content (src/config/ConfigStore.cpp), so a running build picks the edit up
within a frame: turn auto-save on, drag a slider, watch the sim change.

What the page knows that a generic JSON editor does not:
  * per-field docs, scraped from the IMMUNE_CONFIG_FIELD tables in src/, shown
    as tooltips;
  * enum spellings, scraped from the EnumEntry tables in src/, shown as
    dropdowns;
  * integer vs float. The loaders reject `130.0` for a u32 field, so a value
    that was an integer on disk stays an integer on the way back;
  * arrays of same-shaped objects (a tower's tiers) render as a table, one
    column per tier, so a curve across tiers can be read at a glance.

Written files match ConfigStore::write_file byte for byte where it matters:
sorted keys, two-space indent, LF, trailing newline. A save therefore diffs
as only the fields you changed.

Deliberately stdlib-only, same rule as tools/bench_report.py.

Usage:
    python tools/config_editor.py                # opens http://127.0.0.1:8765
    python tools/config_editor.py --port 9000 --no-browser
    python tools/config_editor.py --config path/to/other/config/dir
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG_DIR = REPO / "assets" / "config"
SRC_DIR = REPO / "src"

# Keys the loaders treat as fixed; the UI shows them but refuses to edit them.
READONLY_KEYS = {"schema", "kind"}


# ---------------------------------------------------------------------------
# Metadata scraped from the C++ schema tables
# ---------------------------------------------------------------------------

_FIELD_RE = re.compile(
    r"IMMUNE_CONFIG_(?:ENUM_)?FIELD\(\s*([\w:]+)\s*,\s*(\w+)\s*,\s*FieldKind::(\w+)\s*,"
    r'\s*"((?:[^"\\]|\\.)*)"\s*(?:,\s*(\w+))?\s*\)'
)
_ENUM_TABLE_RE = re.compile(
    r"EnumEntry\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\};", re.S
)
_ENUM_ENTRY_RE = re.compile(r'\{\s*"([^"]+)"\s*,')


def snake_case(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


def scrape_metadata(src_dir: Path) -> dict[str, Any]:
    """Docs and enum spellings keyed by JSON field name.

    JSON keys are the C++ member names verbatim (config/Field.h), so a lookup
    by member name is exact. Where two structs share a member name with
    different docs both are kept, joined; the tooltip is still useful.
    """
    docs: dict[str, list[str]] = {}
    by_struct: dict[str, dict[str, str]] = {}
    kinds: dict[str, set[str]] = {}
    enum_ref: dict[str, str] = {}
    enum_tables: dict[str, list[str]] = {}

    if not src_dir.is_dir():
        return {"docs": {}, "docs_by_struct": {}, "kinds": {}, "enums": {}}

    for path in src_dir.rglob("*"):
        if path.suffix not in (".cpp", ".h"):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in _FIELD_RE.finditer(text):
            struct, member, kind, doc, enum_name = m.groups()
            doc = doc.replace('\\"', '"')
            by_struct.setdefault(snake_case(struct.split("::")[-1]), {})[member] = doc
            docs.setdefault(member, [])
            if doc and doc not in docs[member]:
                docs[member].append(doc)
            kinds.setdefault(member, set()).add(kind)
            if enum_name:
                enum_ref[member] = enum_name
        for m in _ENUM_TABLE_RE.finditer(text):
            name, body = m.groups()
            enum_tables[name] = _ENUM_ENTRY_RE.findall(body)

    enums: dict[str, list[str]] = {}
    for member, table in enum_ref.items():
        if table in enum_tables:
            enums[member] = enum_tables[table]
    # speed_tier is read with require_enum rather than a schema field.
    if "speed_tier" not in enums and "kSpeedTierEnum" in enum_tables:
        enums["speed_tier"] = enum_tables["kSpeedTierEnum"]

    return {
        "docs": {k: " / ".join(v) for k, v in docs.items() if v},
        "docs_by_struct": by_struct,
        "kinds": {k: sorted(v) for k, v in kinds.items()},
        "enums": enums,
    }


# ---------------------------------------------------------------------------
# File IO
# ---------------------------------------------------------------------------

def read_config(config_dir: Path, name: str) -> Any:
    return json.loads((config_dir / name).read_text(encoding="utf-8"))


def dump_config(document: Any) -> str:
    # Matches nlohmann's dump(2) for everything these files contain: sorted
    # keys, two-space indent, shortest float repr, LF, trailing newline.
    return json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def coerce_types(original: Any, updated: Any) -> Any:
    """Re-applies the on-disk number types to an edited document.

    JavaScript has one number type; the loaders have several. A field that
    was an int on disk comes back as an int (rounded), a float as a float.
    Shape changes (a key the original lacked, a longer list) pass through as
    given.
    """
    if isinstance(original, bool) or isinstance(updated, bool):
        return updated
    if isinstance(original, dict) and isinstance(updated, dict):
        return {k: coerce_types(original.get(k), v) for k, v in updated.items()}
    if isinstance(original, list) and isinstance(updated, list):
        return [
            coerce_types(original[i] if i < len(original) else None, v)
            for i, v in enumerate(updated)
        ]
    if isinstance(updated, (int, float)) and not isinstance(updated, bool):
        if isinstance(original, bool):
            return updated
        if isinstance(original, int):
            return int(round(updated))
        if isinstance(original, float):
            return float(updated)
    return updated


def git_diff(config_dir: Path, name: str) -> str:
    try:
        out = subprocess.run(
            ["git", "diff", "--no-color", "--", str(config_dir / name)],
            cwd=REPO, capture_output=True, text=True, timeout=10,
        )
        return out.stdout if out.returncode == 0 else out.stderr
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"(git unavailable: {e})"


def git_revert(config_dir: Path, name: str) -> tuple[bool, str]:
    try:
        out = subprocess.run(
            ["git", "checkout", "--", str(config_dir / name)],
            cwd=REPO, capture_output=True, text=True, timeout=10,
        )
        return out.returncode == 0, out.stderr.strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, str(e)


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    config_dir: Path = DEFAULT_CONFIG_DIR
    metadata: dict[str, Any] = {}

    def log_message(self, fmt: str, *args: Any) -> None:  # quieter than default
        if "/api/" in (args[0] if args else ""):
            return
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    # -- helpers ------------------------------------------------------------
    def _send(self, status: int, body: bytes, ctype: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, payload: Any, status: int = 200) -> None:
        self._send(status, json.dumps(payload).encode("utf-8"), "application/json")

    def _read_json(self) -> Any:
        length = int(self.headers.get("Content-Length", "0"))
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def _valid_name(self, name: str) -> bool:
        return bool(re.fullmatch(r"[\w.-]+\.json", name)) and (self.config_dir / name).is_file()

    # -- routes -------------------------------------------------------------
    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        query = dict(p.split("=", 1) for p in self.path.split("?", 1)[1].split("&") if "=" in p) \
            if "?" in self.path else {}
        if path == "/":
            self._send(200, PAGE.encode("utf-8"), "text/html; charset=utf-8")
        elif path == "/api/files":
            names = sorted(p.name for p in self.config_dir.glob("*.json"))
            files = {}
            errors = {}
            for n in names:
                try:
                    files[n] = read_config(self.config_dir, n)
                except (OSError, ValueError) as e:
                    errors[n] = str(e)
            self._json({
                "dir": str(self.config_dir),
                "files": files,
                "errors": errors,
                "meta": self.metadata,
                "readonly": sorted(READONLY_KEYS),
            })
        elif path == "/api/diff":
            name = query.get("name", "")
            if not self._valid_name(name):
                self._json({"error": "unknown file"}, 404)
                return
            self._json({"diff": git_diff(self.config_dir, name)})
        else:
            self._send(404, b"not found", "text/plain")

    def do_POST(self) -> None:
        path = self.path.split("?", 1)[0]
        try:
            body = self._read_json()
        except (ValueError, KeyError) as e:
            self._json({"error": f"bad request: {e}"}, 400)
            return

        if path == "/api/save":
            name = body.get("name", "")
            if not self._valid_name(name):
                self._json({"error": "unknown file"}, 404)
                return
            try:
                original = read_config(self.config_dir, name)
            except (OSError, ValueError):
                original = None
            document = coerce_types(original, body.get("document"))
            if not isinstance(document, dict):
                self._json({"error": "top level must be an object"}, 400)
                return
            text = dump_config(document)
            target = self.config_dir / name
            # Write whole-file then rename so the game's content poll never
            # sees a half-written file.
            tmp = target.with_suffix(target.suffix + ".tmp")
            try:
                tmp.write_text(text, encoding="utf-8", newline="\n")
                os.replace(tmp, target)
            except OSError as e:
                self._json({"error": f"write failed: {e}"}, 500)
                return
            self._json({"ok": True, "document": document})

        elif path == "/api/revert":
            name = body.get("name", "")
            if not self._valid_name(name):
                self._json({"error": "unknown file"}, 404)
                return
            ok, err = git_revert(self.config_dir, name)
            if not ok:
                self._json({"error": err or "git checkout failed"}, 500)
                return
            self._json({"ok": True, "document": read_config(self.config_dir, name)})

        else:
            self._send(404, b"not found", "text/plain")


# ---------------------------------------------------------------------------
# The page
# ---------------------------------------------------------------------------

PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Config Tuner</title>
<style>
  :root {
    --bg: #14161a; --panel: #1c1f25; --panel2: #23272f; --line: #2e333d;
    --fg: #e6e8ec; --muted: #8b93a1; --accent: #5fb3ff; --accent2: #7ee2a8;
    --warn: #ffb454; --danger: #ff6b6b; --dirty: #ffd166;
    --mono: ui-monospace, "Cascadia Code", Consolas, monospace;
    --sans: system-ui, "Segoe UI", sans-serif;
  }
  * { box-sizing: border-box; }
  html, body { height: 100%; margin: 0; }
  body { background: var(--bg); color: var(--fg); font: 13px/1.4 var(--sans); display: flex; flex-direction: column; }

  header { display: flex; align-items: center; gap: 12px; padding: 8px 14px; background: var(--panel); border-bottom: 1px solid var(--line); flex-wrap: wrap; }
  header h1 { font-size: 14px; margin: 0; font-weight: 600; letter-spacing: .02em; }
  header .dir { color: var(--muted); font-family: var(--mono); font-size: 11px; }
  header .spacer { flex: 1; }
  header input[type=search] { background: var(--bg); border: 1px solid var(--line); color: var(--fg); padding: 5px 8px; border-radius: 6px; width: 220px; }
  .btn { background: var(--panel2); border: 1px solid var(--line); color: var(--fg); padding: 5px 10px; border-radius: 6px; cursor: pointer; font: inherit; }
  .btn:hover { border-color: var(--accent); }
  .btn.primary { background: var(--accent); color: #0b1620; border-color: var(--accent); font-weight: 600; }
  .btn.primary:disabled { opacity: .4; cursor: default; }
  .btn.danger:hover { border-color: var(--danger); color: var(--danger); }
  label.toggle { display: inline-flex; align-items: center; gap: 6px; cursor: pointer; color: var(--muted); }
  label.toggle input { accent-color: var(--accent2); }

  main { display: flex; flex: 1; min-height: 0; }
  nav { width: 190px; background: var(--panel); border-right: 1px solid var(--line); padding: 8px; overflow-y: auto; flex-shrink: 0; }
  nav .file { display: flex; align-items: center; gap: 6px; padding: 7px 10px; border-radius: 6px; cursor: pointer; color: var(--muted); font-family: var(--mono); font-size: 12px; }
  nav .file:hover { background: var(--panel2); color: var(--fg); }
  nav .file.active { background: var(--panel2); color: var(--fg); box-shadow: inset 2px 0 0 var(--accent); }
  nav .file .dot { width: 7px; height: 7px; border-radius: 50%; background: transparent; margin-left: auto; }
  nav .file.dirty .dot { background: var(--dirty); }

  #content { flex: 1; overflow-y: auto; padding: 14px 18px 60px; }
  #content.diffmode pre { white-space: pre; font-family: var(--mono); font-size: 12px; background: var(--panel); padding: 12px; border-radius: 8px; border: 1px solid var(--line); overflow-x: auto; }
  .diff-add { color: var(--accent2); } .diff-del { color: var(--danger); } .diff-hdr { color: var(--muted); }

  details.sec { border: 1px solid var(--line); border-radius: 8px; background: var(--panel); margin-bottom: 10px; }
  details.sec > summary { cursor: pointer; padding: 8px 12px; font-weight: 600; font-family: var(--mono); font-size: 12.5px; list-style: none; display: flex; align-items: center; gap: 8px; user-select: none; }
  details.sec > summary::before { content: "▸"; color: var(--muted); font-size: 11px; transition: transform .12s; }
  details.sec[open] > summary::before { transform: rotate(90deg); }
  details.sec > summary .count { color: var(--muted); font-weight: 400; font-size: 11px; }
  details.sec > .body { padding: 4px 12px 10px; }
  details.sec details.sec { background: var(--panel2); margin: 6px 0; }
  details.sec details.sec details.sec { background: var(--panel); }

  .field { display: grid; grid-template-columns: 200px 1fr; align-items: center; gap: 10px; padding: 4px 0; border-bottom: 1px dashed transparent; }
  .field:hover { background: rgba(255,255,255,.02); }
  .field.dirty .key { color: var(--dirty); }
  .field.dirty .key::after { content: " •"; }
  .field .key { font-family: var(--mono); font-size: 12px; color: var(--fg); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; cursor: help; }
  .field .key.ro { color: var(--muted); cursor: default; }
  .field .ctl { display: flex; align-items: center; gap: 8px; min-width: 0; }
  input[type=range] { flex: 1; min-width: 80px; accent-color: var(--accent); height: 4px; }
  input.num { width: 92px; background: var(--bg); border: 1px solid var(--line); color: var(--fg); padding: 4px 6px; border-radius: 5px; font-family: var(--mono); font-size: 12px; text-align: right; }
  input.num:focus, input.txt:focus, select:focus { outline: none; border-color: var(--accent); }
  input.num.int { border-style: dotted; }
  input.txt { flex: 1; background: var(--bg); border: 1px solid var(--line); color: var(--fg); padding: 4px 6px; border-radius: 5px; font-family: var(--mono); font-size: 12px; }
  select { background: var(--bg); border: 1px solid var(--line); color: var(--fg); padding: 4px 6px; border-radius: 5px; font-family: var(--mono); font-size: 12px; }
  .ro-val { color: var(--muted); font-family: var(--mono); font-size: 12px; }
  .range-hint { color: var(--muted); font-family: var(--mono); font-size: 10px; width: 46px; text-align: right; }
  input[type=color] { width: 34px; height: 24px; border: 1px solid var(--line); background: none; padding: 0; border-radius: 5px; cursor: pointer; }
  .swatch { width: 24px; height: 24px; border-radius: 5px; border: 1px solid var(--line); }
  .bool { accent-color: var(--accent2); width: 16px; height: 16px; }
  .doc { color: var(--muted); font-size: 11px; grid-column: 2; margin-top: -2px; }

  table.tiers { border-collapse: collapse; width: 100%; margin: 6px 0; }
  table.tiers th { text-align: left; font-family: var(--mono); font-size: 11px; color: var(--muted); padding: 4px 8px; border-bottom: 1px solid var(--line); font-weight: 500; }
  table.tiers th:first-child { width: 190px; }
  table.tiers td { padding: 3px 8px; vertical-align: middle; }
  table.tiers tr.group td { font-family: var(--mono); font-size: 12px; font-weight: 600; color: var(--accent); padding-top: 10px; }
  table.tiers tr.row td.key { font-family: var(--mono); font-size: 12px; cursor: help; }
  table.tiers tr.row td.key.ro { color: var(--muted); cursor: default; }
  table.tiers tr.row:hover td { background: rgba(255,255,255,.02); }
  table.tiers td.cell { min-width: 150px; }
  table.tiers td.cell.dirty { box-shadow: inset 2px 0 0 var(--dirty); }
  table.tiers td.cell .ctl { display: flex; align-items: center; gap: 6px; }
  table.tiers td.cell input[type=range] { min-width: 50px; }
  table.tiers td.cell .range-hint { display: none; }
  table.tiers td.cell input.num { width: 72px; }
  .tablewrap { overflow-x: auto; }

  .empty { color: var(--muted); font-style: italic; }
  .hidden { display: none !important; }

  #toast { position: fixed; bottom: 16px; right: 16px; background: var(--panel2); border: 1px solid var(--line); padding: 8px 12px; border-radius: 8px; opacity: 0; transition: opacity .15s; pointer-events: none; font-family: var(--mono); font-size: 12px; max-width: 480px; }
  #toast.show { opacity: 1; }
  #toast.err { border-color: var(--danger); color: var(--danger); }
  #toast.ok { border-color: var(--accent2); }
  kbd { font-family: var(--mono); background: var(--bg); border: 1px solid var(--line); padding: 0 4px; border-radius: 3px; font-size: 11px; }
  @media (max-width: 760px) { nav { width: 120px; } .field { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<header>
  <h1>Config Tuner</h1>
  <span class="dir" id="dir"></span>
  <span class="spacer"></span>
  <input type="search" id="filter" placeholder="filter fields…  (/)">
  <label class="toggle" title="Write the file on every change. The game hot-reloads it."><input type="checkbox" id="autosave"> auto-save</label>
  <label class="toggle" title="Show the field docs from the C++ schema under every field."><input type="checkbox" id="showdocs"> docs</label>
  <button class="btn" id="expand">expand all</button>
  <button class="btn" id="collapse">collapse all</button>
  <button class="btn" id="diffbtn" title="git diff for this file">diff</button>
  <button class="btn" id="discard" title="Throw away unsaved edits">discard</button>
  <button class="btn danger" id="revert" title="git checkout this file (loses saved edits)">revert to HEAD</button>
  <button class="btn primary" id="save" disabled title="Ctrl+S">save</button>
</header>
<main>
  <nav id="files"></nav>
  <div id="content"></div>
</main>
<div id="toast"></div>

<script>
(() => {
  'use strict';
  const $ = (s, r = document) => r.querySelector(s);
  const state = {
    files: {},      // name -> working document
    saved: {},      // name -> last document known to be on disk
    meta: { docs: {}, kinds: {}, enums: {} },
    readonly: new Set(),
    current: null,
    diffMode: false,
    autosaveTimer: null,
  };

  // ---------------------------------------------------------------- utils
  const clone = (v) => JSON.parse(JSON.stringify(v));
  const isObj = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);
  const deepEq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  const getPath = (doc, path) => path.reduce((o, k) => (o == null ? undefined : o[k]), doc);
  const setPath = (doc, path, v) => { let o = doc; for (let i = 0; i < path.length - 1; i++) o = o[path[i]]; o[path[path.length - 1]] = v; };
  const fmtNum = (v) => Number.isInteger(v) ? String(v) : String(+v.toPrecision(6));
  const toast = (msg, kind = '') => {
    const t = $('#toast'); t.textContent = msg; t.className = 'show ' + kind;
    clearTimeout(toast._t); toast._t = setTimeout(() => t.className = '', 1800);
  };
  const isDirty = (name) => !deepEq(state.files[name], state.saved[name]);

  // Number type is decided by what was on disk: an int stays an int.
  const isIntField = (name, path) => {
    const v = getPath(state.saved[name], path);
    if (typeof v === 'number') return Number.isInteger(v) && !isFloatKind(path[path.length - 1]);
    return false;
  };
  const isFloatKind = (key) => {
    const k = state.meta.kinds[key];
    return !!k && k.every(x => x === 'F32' || x === 'F64' || x === 'Vec2' || x === 'Vec4');
  };
  const isIntKind = (key) => {
    const k = state.meta.kinds[key];
    return !!k && k.every(x => x === 'U32' || x === 'U64' || x === 'I32' || x === 'U8');
  };
  const intField = (name, path) => isIntKind(path[path.length - 1]) || isIntField(name, path);

  // ---------------------------------------------------------------- api
  async function api(path, body) {
    const r = await fetch(path, body ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) } : {});
    const j = await r.json();
    if (!r.ok || j.error) throw new Error(j.error || r.statusText);
    return j;
  }

  async function load() {
    const j = await api('/api/files');
    state.files = j.files; state.saved = clone(j.files); state.meta = j.meta;
    state.readonly = new Set(j.readonly);
    $('#dir').textContent = j.dir;
    for (const [n, e] of Object.entries(j.errors || {})) toast(`${n}: ${e}`, 'err');
    if (!state.current || !(state.current in state.files)) state.current = Object.keys(state.files)[0] || null;
    renderNav(); render();
  }

  async function save(name = state.current) {
    if (!name || !isDirty(name)) return;
    try {
      const sent = clone(state.files[name]);
      const j = await api('/api/save', { name, document: sent });
      state.saved[name] = clone(j.document);
      state.files[name] = clone(j.document);
      toast(`saved ${name}`, 'ok');
      renderNav();
      if (!deepEq(sent, j.document) && name === state.current) render(); else refreshDirtyMarks();
    } catch (e) { toast(`save failed: ${e.message}`, 'err'); }
  }

  function scheduleAutosave() {
    if (!$('#autosave').checked) return;
    clearTimeout(state.autosaveTimer);
    state.autosaveTimer = setTimeout(() => save(), 150);
  }

  // ---------------------------------------------------------------- nav
  function renderNav() {
    const nav = $('#files'); nav.innerHTML = '';
    for (const name of Object.keys(state.files)) {
      const d = document.createElement('div');
      d.className = 'file' + (name === state.current ? ' active' : '') + (isDirty(name) ? ' dirty' : '');
      d.innerHTML = `<span>${name}</span><span class="dot"></span>`;
      d.onclick = () => { state.current = name; state.diffMode = false; renderNav(); render(); };
      nav.appendChild(d);
    }
    $('#save').disabled = !state.current || !isDirty(state.current);
  }

  // ---------------------------------------------------------------- change plumbing
  function commit(name, path, value) {
    setPath(state.files[name], path, value);
    renderNav();
    refreshDirtyMarks();
    scheduleAutosave();
  }

  function refreshDirtyMarks() {
    const name = state.current; if (!name) return;
    for (const el of document.querySelectorAll('[data-path]')) {
      const path = JSON.parse(el.dataset.path);
      const dirty = !deepEq(getPath(state.files[name], path), getPath(state.saved[name], path));
      el.classList.toggle('dirty', dirty);
    }
  }

  // ---------------------------------------------------------------- controls
  function sliderRange(v, isInt) {
    // A slider needs bounds and JSON has none, so derive them from the value:
    // roomy enough to double it, re-derived when the number box overshoots.
    const a = Math.abs(v);
    let max = a === 0 ? (isInt ? 10 : 1) : a * 2;
    let min = v < 0 ? -max : 0;
    if (isInt) { max = Math.ceil(max); min = Math.floor(min); }
    const step = isInt ? 1 : +((max - min) / 400).toPrecision(2);
    return { min, max, step };
  }

  function numberControl(name, path, value, key) {
    const isInt = intField(name, path);
    const wrap = document.createElement('div'); wrap.className = 'ctl';
    const range = document.createElement('input'); range.type = 'range';
    const num = document.createElement('input'); num.type = 'number'; num.className = 'num' + (isInt ? ' int' : '');
    const hint = document.createElement('span'); hint.className = 'range-hint';
    const applyRange = (v) => {
      const r = sliderRange(v, isInt);
      range.min = r.min; range.max = r.max; range.step = r.step; hint.textContent = fmtNum(r.max);
    };
    applyRange(value);
    range.value = value; num.value = fmtNum(value);
    if (isInt) num.step = 1; else num.step = 'any';
    num.title = isInt ? 'integer field' : 'float field';

    range.oninput = () => { const v = isInt ? Math.round(+range.value) : +range.value; num.value = fmtNum(v); commit(name, path, v); };
    range.onchange = () => applyRange(+range.value);  // re-centre after the drag ends
    num.onchange = () => {
      let v = +num.value; if (!Number.isFinite(v)) { num.value = fmtNum(getPath(state.files[name], path)); return; }
      if (isInt) v = Math.round(v);
      num.value = fmtNum(v); applyRange(v); range.value = v; commit(name, path, v);
    };
    // Drag-to-scrub on the number box, like an ImGui DragFloat.
    let drag = null;
    num.onpointerdown = (e) => { if (e.button !== 0) return; drag = { x: e.clientX, v: +num.value, moved: false }; };
    num.onpointermove = (e) => {
      if (!drag) return;
      const dx = e.clientX - drag.x;
      if (!drag.moved && Math.abs(dx) < 4) return;
      if (!drag.moved) { drag.moved = true; num.setPointerCapture(e.pointerId); num.blur(); }
      const scale = e.shiftKey ? 10 : (e.altKey ? 0.1 : 1);
      const base = Math.max(Math.abs(drag.v), isInt ? 1 : 0.01);
      let v = drag.v + dx * base * 0.005 * scale;
      if (isInt) v = Math.round(v); else v = +v.toPrecision(5);
      num.value = fmtNum(v); range.value = v; commit(name, path, v);
    };
    num.onpointerup = (e) => { if (drag && drag.moved) { applyRange(+num.value); e.preventDefault(); } drag = null; };
    wrap.append(range, hint, num);
    return wrap;
  }

  function boolControl(name, path, value) {
    const wrap = document.createElement('div'); wrap.className = 'ctl';
    const cb = document.createElement('input'); cb.type = 'checkbox'; cb.className = 'bool'; cb.checked = !!value;
    cb.onchange = () => commit(name, path, cb.checked);
    wrap.appendChild(cb); return wrap;
  }

  function stringControl(name, path, value, key) {
    const wrap = document.createElement('div'); wrap.className = 'ctl';
    const opts = state.meta.enums[key];
    if (opts && opts.length) {
      const sel = document.createElement('select');
      const all = opts.includes(value) ? opts : [value, ...opts];
      for (const o of all) { const op = document.createElement('option'); op.value = o; op.textContent = o; sel.appendChild(op); }
      sel.value = value; sel.onchange = () => commit(name, path, sel.value);
      wrap.appendChild(sel);
    } else {
      const inp = document.createElement('input'); inp.type = 'text'; inp.className = 'txt'; inp.value = value;
      inp.onchange = () => commit(name, path, inp.value);
      wrap.appendChild(inp);
    }
    return wrap;
  }

  const toHex = (c) => '#' + [0, 1, 2].map(i => Math.round(Math.min(1, Math.max(0, c[i])) * 255).toString(16).padStart(2, '0')).join('');
  const fromHex = (h) => [1, 3, 5].map(i => +(parseInt(h.slice(i, i + 2), 16) / 255).toFixed(4));

  function colorControl(name, path, value) {
    const wrap = document.createElement('div'); wrap.className = 'ctl';
    const pick = document.createElement('input'); pick.type = 'color'; pick.value = toHex(value);
    const nums = [];
    const sync = () => { const v = getPath(state.files[name], path); pick.value = toHex(v); nums.forEach((n, i) => n.value = fmtNum(v[i])); };
    pick.oninput = () => { const rgb = fromHex(pick.value); const v = getPath(state.files[name], path).slice(); rgb.forEach((c, i) => v[i] = c); commit(name, path, v); sync(); };
    wrap.appendChild(pick);
    ['r', 'g', 'b', 'a'].forEach((lbl, i) => {
      if (i >= value.length) return;
      const n = document.createElement('input'); n.type = 'number'; n.className = 'num'; n.step = 'any'; n.style.width = '64px';
      n.value = fmtNum(value[i]); n.title = lbl;
      n.onchange = () => { const v = getPath(state.files[name], path).slice(); v[i] = +n.value; commit(name, path, v); sync(); };
      nums.push(n); wrap.appendChild(n);
    });
    return wrap;
  }

  function vecControl(name, path, value) {
    const wrap = document.createElement('div'); wrap.className = 'ctl';
    value.forEach((c, i) => {
      const n = document.createElement('input'); n.type = 'number'; n.className = 'num'; n.step = 'any';
      n.value = fmtNum(c); n.title = ['x', 'y', 'z', 'w'][i] || String(i);
      n.onchange = () => { const v = getPath(state.files[name], path).slice(); v[i] = +n.value; commit(name, path, v); };
      wrap.appendChild(n);
    });
    return wrap;
  }

  function leafControl(name, path, value, key) {
    if (state.readonly.has(key)) { const s = document.createElement('span'); s.className = 'ro-val'; s.textContent = JSON.stringify(value); return s; }
    if (typeof value === 'boolean') return boolControl(name, path, value);
    if (typeof value === 'number') return numberControl(name, path, value, key);
    if (typeof value === 'string') return stringControl(name, path, value, key);
    if (Array.isArray(value) && value.every(x => typeof x === 'number')) {
      if (value.length === 4 && /colou?r/.test(key)) return colorControl(name, path, value);
      if (value.length >= 2 && value.length <= 4) return vecControl(name, path, value);
    }
    if (Array.isArray(value) && value.length === 0) { const s = document.createElement('span'); s.className = 'empty'; s.textContent = '(empty list)'; return s; }
    const s = document.createElement('span'); s.className = 'ro-val'; s.textContent = JSON.stringify(value); return s;
  }

  const isLeaf = (v) => !isObj(v) && !(Array.isArray(v) && v.length && v.every(isObj));

  // ---------------------------------------------------------------- rendering
  // The struct that owns a field is unknown from JSON alone, but its snake-cased
  // name usually contains the parent key (FamilyDeathVfx <-> death_vfx), which
  // picks the right doc when several structs share a member name.
  function docFor(key, parentKey) {
    const bs = state.meta.docs_by_struct || {};
    if (parentKey) {
      const pk = String(parentKey).toLowerCase();
      for (const [sname, members] of Object.entries(bs)) {
        if (members[key] !== undefined && (sname.includes(pk) || pk.includes(sname))) return members[key];
      }
    }
    return state.meta.docs[key];
  }

  function keyLabel(key, tag = 'span', parentKey = null) {
    const k = document.createElement(tag); k.className = 'key' + (state.readonly.has(key) ? ' ro' : ''); k.textContent = key;
    const doc = docFor(key, parentKey); if (doc) k.title = doc;
    return k;
  }

  function fieldRow(name, path, key, value) {
    const row = document.createElement('div'); row.className = 'field'; row.dataset.path = JSON.stringify(path); row.dataset.key = key;
    const parentKey = path.length > 1 ? path[path.length - 2] : null;
    row.appendChild(keyLabel(key, 'span', parentKey));
    row.appendChild(leafControl(name, path, value, key));
    const doc = docFor(key, parentKey);
    if (doc) { const d = document.createElement('div'); d.className = 'doc' + ($('#showdocs').checked ? '' : ' hidden'); d.textContent = doc; row.appendChild(d); }
    return row;
  }

  function section(title, count) {
    const d = document.createElement('details'); d.className = 'sec'; d.open = true;
    const s = document.createElement('summary'); s.textContent = title;
    if (count != null) { const c = document.createElement('span'); c.className = 'count'; c.textContent = count; s.appendChild(c); }
    const body = document.createElement('div'); body.className = 'body';
    d.append(s, body); d.body = body; return d;
  }

  // Same-shaped object arrays (tiers) as a table: one column per element.
  function tierTable(name, path, arr) {
    const table = document.createElement('table'); table.className = 'tiers';
    const thead = document.createElement('thead'); const hr = document.createElement('tr');
    hr.appendChild(document.createElement('th'));
    arr.forEach((_, i) => { const th = document.createElement('th'); th.textContent = `${path[path.length - 1].replace(/s$/, '')} ${i + 1}`; hr.appendChild(th); });
    thead.appendChild(hr); table.appendChild(thead);
    const tbody = document.createElement('tbody');
    const walk = (subPath, depth) => {
      const first = getPath(arr[0], subPath);
      for (const key of Object.keys(first)) {
        const v = first[key]; const p = [...subPath, key];
        if (isObj(v)) {
          const g = document.createElement('tr'); g.className = 'group';
          const td = document.createElement('td'); td.colSpan = arr.length + 1; td.textContent = p.join('.'); g.appendChild(td); tbody.appendChild(g);
          walk(p, depth + 1);
        } else {
          const tr = document.createElement('tr'); tr.className = 'row'; tr.dataset.key = key;
          tr.appendChild(keyLabel(key, 'td', p.length > 1 ? p[p.length - 2] : null));
          arr.forEach((el, i) => {
            const td = document.createElement('td'); td.className = 'cell'; const full = [...path, i, ...p]; td.dataset.path = JSON.stringify(full);
            const val = getPath(el, p);
            td.appendChild(val === undefined ? Object.assign(document.createElement('span'), { className: 'empty', textContent: '—' }) : leafControl(name, full, val, key));
            tr.appendChild(td);
          });
          tbody.appendChild(tr);
        }
      }
    };
    walk([], 0);
    table.appendChild(tbody); return table;
  }

  function renderObject(name, path, obj, into) {
    const leaves = Object.keys(obj).filter(k => isLeaf(obj[k]));
    const groups = Object.keys(obj).filter(k => !isLeaf(obj[k]));
    for (const k of leaves) into.appendChild(fieldRow(name, [...path, k], k, obj[k]));
    for (const k of groups) {
      const v = obj[k];
      if (Array.isArray(v)) {
        const s = section(k, `${v.length} entries`); const w = document.createElement('div'); w.className = 'tablewrap'; w.appendChild(tierTable(name, [...path, k], v)); s.body.appendChild(w); into.appendChild(s);
      } else {
        const s = section(k, `${Object.keys(v).length}`); renderObject(name, [...path, k], v, s.body); into.appendChild(s);
      }
    }
  }

  function render() {
    const c = $('#content'); c.innerHTML = ''; c.classList.toggle('diffmode', state.diffMode);
    const name = state.current; if (!name) { c.innerHTML = '<p class="empty">no config files found</p>'; return; }
    if (state.diffMode) { renderDiff(name, c); return; }
    renderObject(name, [], state.files[name], c);
    applyFilter(); refreshDirtyMarks();
  }

  async function renderDiff(name, into) {
    into.innerHTML = '<p class="empty">loading diff…</p>';
    try {
      const j = await api('/api/diff?name=' + encodeURIComponent(name));
      const pre = document.createElement('pre');
      if (!j.diff.trim()) { pre.textContent = `(no diff vs HEAD for ${name}${isDirty(name) ? ' — you have unsaved edits' : ''})`; }
      else for (const line of j.diff.split('\n')) {
        const s = document.createElement('span');
        s.className = line.startsWith('+') && !line.startsWith('+++') ? 'diff-add' : line.startsWith('-') && !line.startsWith('---') ? 'diff-del' : /^(diff|index|@@|\+\+\+|---)/.test(line) ? 'diff-hdr' : '';
        s.textContent = line + '\n'; pre.appendChild(s);
      }
      into.innerHTML = ''; into.appendChild(pre);
    } catch (e) { into.innerHTML = `<p class="empty">${e.message}</p>`; }
  }

  function applyFilter() {
    const q = $('#filter').value.trim().toLowerCase();
    for (const el of document.querySelectorAll('.field, tr.row')) {
      const hay = (el.dataset.key || '') + ' ' + (el.closest('details.sec')?.querySelector('summary')?.textContent || '');
      el.classList.toggle('hidden', !!q && !hay.toLowerCase().includes(q));
    }
    for (const sec of [...document.querySelectorAll('details.sec')].reverse()) {
      const anyVisible = sec.querySelector('.field:not(.hidden), tr.row:not(.hidden)');
      sec.classList.toggle('hidden', !!q && !anyVisible);
      if (q && anyVisible) sec.open = true;
    }
  }

  // ---------------------------------------------------------------- wiring
  $('#save').onclick = () => save();
  $('#discard').onclick = () => { const n = state.current; if (!n || !isDirty(n)) return; state.files[n] = clone(state.saved[n]); renderNav(); render(); toast('discarded edits'); };
  $('#revert').onclick = async () => {
    const n = state.current; if (!n) return;
    if (!confirm(`git checkout -- ${n}\n\nThis discards every change to the file since the last commit, including saved ones. Continue?`)) return;
    try { const j = await api('/api/revert', { name: n }); state.saved[n] = clone(j.document); state.files[n] = clone(j.document); renderNav(); render(); toast(`reverted ${n}`, 'ok'); }
    catch (e) { toast(`revert failed: ${e.message}`, 'err'); }
  };
  $('#diffbtn').onclick = () => { state.diffMode = !state.diffMode; $('#diffbtn').classList.toggle('primary', state.diffMode); render(); };
  $('#expand').onclick = () => document.querySelectorAll('details.sec').forEach(d => d.open = true);
  $('#collapse').onclick = () => document.querySelectorAll('details.sec').forEach(d => d.open = false);
  $('#filter').oninput = applyFilter;
  $('#showdocs').onchange = () => document.querySelectorAll('.doc').forEach(d => d.classList.toggle('hidden', !$('#showdocs').checked));
  $('#autosave').onchange = () => { if ($('#autosave').checked) save(); };
  document.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key === 's') { e.preventDefault(); save(); }
    if (e.key === '/' && document.activeElement.tagName !== 'INPUT') { e.preventDefault(); $('#filter').focus(); }
    if (e.key === 'Escape' && document.activeElement === $('#filter')) { $('#filter').value = ''; applyFilter(); $('#filter').blur(); }
  });
  window.addEventListener('beforeunload', (e) => { if (Object.keys(state.files).some(isDirty)) { e.preventDefault(); e.returnValue = ''; } });

  // The game's gym console (`config dump`) and other editors write these
  // files too. Poll the disk so the page never shows a stale value, and never
  // clobbers an external edit with a stale save.
  async function pollDisk() {
    try {
      const j = await api('/api/files');
      let changedCurrent = false;
      for (const [name, doc] of Object.entries(j.files)) {
        if (deepEq(doc, state.saved[name])) continue;
        const wasDirty = name in state.saved && isDirty(name);
        if (!(name in state.saved)) { state.saved[name] = clone(doc); state.files[name] = clone(doc); changedCurrent = true; continue; }
        if (wasDirty) {
          // Keep the local edits, but re-base them on the new disk contents
          // so the dirty marks and the next save are relative to what is there.
          const local = state.files[name], base = state.saved[name], merged = clone(doc);
          const walk = (path, l, b) => {
            if (isObj(l) && isObj(b)) { for (const k of Object.keys(l)) walk([...path, k], l[k], b[k]); return; }
            if (Array.isArray(l) && Array.isArray(b) && l.length && l.every(isObj)) { l.forEach((x, i) => walk([...path, i], x, b[i])); return; }
            if (!deepEq(l, b) && getPath(merged, path) !== undefined) setPath(merged, path, l);
          };
          walk([], local, base);
          state.files[name] = merged;
          toast(`${name} changed on disk; your unsaved edits were kept`, '');
        } else {
          state.files[name] = clone(doc);
        }
        state.saved[name] = clone(doc);
        if (name === state.current) changedCurrent = true;
      }
      for (const name of Object.keys(state.files)) if (!(name in j.files)) { delete state.files[name]; delete state.saved[name]; }
      if (changedCurrent && !state.diffMode && !document.activeElement?.closest?.('#content')) { renderNav(); render(); }
      else if (changedCurrent) renderNav();
    } catch (e) { /* server gone; the next tick retries */ }
  }
  setInterval(pollDisk, 1500);

  load().catch(e => toast(e.message, 'err'));
})();
</script>
</body>
</html>
"""


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--config", type=Path, default=DEFAULT_CONFIG_DIR,
                    help="config directory to edit (default: assets/config)")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--no-browser", action="store_true", help="do not open a browser tab")
    args = ap.parse_args(argv)

    config_dir = args.config.resolve()
    if not config_dir.is_dir():
        print(f"config_editor: not a directory: {config_dir}", file=sys.stderr)
        return 2

    Handler.config_dir = config_dir
    Handler.metadata = scrape_metadata(SRC_DIR)
    print(f"config_editor: {len(Handler.metadata['docs'])} field docs, "
          f"{len(Handler.metadata['enums'])} enums scraped from {SRC_DIR}")

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    url = f"http://{args.host}:{args.port}/"
    print(f"config_editor: editing {config_dir}\nconfig_editor: open {url}  (Ctrl+C to stop)")
    if not args.no_browser:
        threading.Timer(0.3, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
