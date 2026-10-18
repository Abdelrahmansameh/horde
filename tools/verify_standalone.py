"""Verify the published EXE with no source tree, loose assets, or runtime DLLs.

Uses only the Python standard library. Execution and evidence directories are
preserved on success and failure so screenshots and logs remain reviewable.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import tempfile
import time
import zlib


LEVEL = "immune-embedded:/assets/levels/campaign_01_first_bend.json"
SYSTEM_DLLS = {
    "advapi32.dll", "avrt.dll", "bcrypt.dll", "cfgmgr32.dll", "comctl32.dll",
    "comdlg32.dll", "crypt32.dll", "d3d9.dll", "d3d11.dll", "dbghelp.dll",
    "dinput8.dll", "dwmapi.dll", "dxgi.dll", "gdi32.dll", "hid.dll",
    "imm32.dll", "kernel32.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll",
    "opengl32.dll", "powrprof.dll", "propsys.dll", "rpcrt4.dll", "secur32.dll",
    "setupapi.dll", "shell32.dll", "shlwapi.dll", "user32.dll", "uxtheme.dll",
    "version.dll", "winmm.dll", "wintrust.dll", "ws2_32.dll",
}
BAD_IMPORT = re.compile(r"^(?:sdl2|msvcp|vcruntime|ucrtbase|api-ms-win-crt)", re.I)
BAD_LOG = re.compile(
    r"\[ERROR\]|\bfall(?:ing\s+back|back)\b|"
    r"shader[^\n]*(?:error|fail)|(?:error|fail)[^\n]*shader|"
    r"\bfailed to (?:load|read|open)\b|--exec failed", re.I)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def inspect_pe(path, system_root):
    """Read PE headers and direct import descriptors without external tools."""
    data = path.read_bytes()

    def read(fmt, offset):
        require(0 <= offset <= len(data) - struct.calcsize(fmt), "Truncated PE header")
        return struct.unpack_from(fmt, data, offset)

    require(data[:2] == b"MZ", "Missing DOS executable signature")
    pe, = read("<I", 0x3C)
    require(data[pe:pe + 4] == b"PE\0\0", "Missing PE signature")
    machine, section_count = read("<HH", pe + 4)
    optional_size, = read("<H", pe + 20)
    optional = pe + 24
    magic, = read("<H", optional)
    subsystem, = read("<H", optional + 68)
    require(machine == 0x8664 and magic == 0x20B, "Executable must be x64 PE32+")
    require(subsystem == 2, "Executable must use the Windows GUI subsystem")
    directory_count, = read("<I", optional + 108)
    require(directory_count >= 2 and optional_size >= 128, "Missing PE import directory")
    sections = []
    for index in range(section_count):
        offset = optional + optional_size + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = read("<IIII", offset + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer, raw_size))

    def rva_offset(rva):
        for address, size, raw, raw_size in sections:
            if address <= rva < address + size:
                require(rva - address < raw_size, "PE RVA references uninitialized section data")
                return raw + rva - address
        raise ValueError(f"Unmapped PE RVA: {rva:#x}")

    import_rva, import_size = read("<II", optional + 120)
    require(import_rva and import_size, "Executable has no direct imports")
    descriptor = rva_offset(import_rva)
    imports = []
    for index in range(min(import_size // 20 + 1, 4096)):
        fields = read("<IIIII", descriptor + index * 20)
        if not any(fields):
            break
        name_start = rva_offset(fields[3])
        name_end = data.find(b"\0", name_start, name_start + 256)
        require(name_end > name_start, "Invalid PE import name")
        name = data[name_start:name_end].decode("ascii").lower()
        require(not BAD_IMPORT.match(name), f"Non-standalone runtime import: {name}")
        require(name in SYSTEM_DLLS, f"Unrecognized non-system import: {name}")
        require((system_root / "System32" / name).is_file(), f"System DLL does not exist: {name}")
        imports.append(name)
    else:
        raise ValueError("Unterminated PE import table")
    require(imports, "Executable has no import descriptors")
    return {"machine": "x64", "subsystem": "Windows GUI", "imports": sorted(set(imports))}


def inspect_png(path, width, height):
    data = path.read_bytes()
    require(data[:8] == b"\x89PNG\r\n\x1a\n", f"Invalid PNG signature: {path.name}")
    offset = 8
    compressed = bytearray()
    image_header = None
    finished = False
    while offset < len(data):
        require(offset + 12 <= len(data), "Truncated PNG chunk")
        length, = struct.unpack_from(">I", data, offset)
        kind = data[offset + 4:offset + 8]
        end = offset + 8 + length
        require(end + 4 <= len(data), "Truncated PNG payload")
        content = data[offset + 8:end]
        checksum, = struct.unpack_from(">I", data, end)
        require(zlib.crc32(kind + content) & 0xFFFFFFFF == checksum, "PNG checksum mismatch")
        if kind == b"IHDR":
            require(length == 13, "Invalid PNG image header")
            image_header = struct.unpack(">IIBBBBB", content)
        elif kind == b"IDAT":
            compressed.extend(content)
        elif kind == b"IEND":
            require(length == 0 and end + 4 == len(data), "Invalid PNG end chunk")
            finished = True
            break
        offset = end + 4
    require(finished and image_header is not None and compressed, "Incomplete PNG")
    actual_width, actual_height, depth, color, compression, filtering, interlace = image_header
    require((actual_width, actual_height) == (width, height), "Unexpected screenshot dimensions")
    require(depth == 8 and color in (2, 6) and compression == filtering == interlace == 0,
            "Unsupported screenshot PNG format")
    channels = 3 if color == 2 else 4
    stride = width * channels + 1
    raw = zlib.decompress(compressed)
    require(len(raw) == height * stride, "Incorrect PNG decompressed size")
    require(all(raw[row * stride] <= 4 for row in range(height)), "Invalid PNG row filter")
    return {"path": str(path), "width": width, "height": height, "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, help="New or empty directory for logs and screenshots")
    args = parser.parse_args()
    require(os.name == "nt", "Standalone verification requires Windows")
    source = args.executable.resolve(strict=True)
    run_dir = Path(tempfile.mkdtemp(prefix="immune-standalone-verify-")).resolve()
    repository = Path(__file__).resolve().parent.parent
    require(not run_dir.is_relative_to(repository), "Execution directory must be outside the repository")
    output = args.output.resolve() if args.output else run_dir
    if output != run_dir:
        require(not output.exists() or not any(output.iterdir()), "Evidence output directory must be empty")
        output.mkdir(parents=True, exist_ok=True)
    executable = run_dir / "IMMUNE.exe"
    shutil.copy2(source, executable)
    require(list(run_dir.iterdir()) == [executable], "Only the executable may be copied for verification")
    system_root = Path(os.environ["SystemRoot"])
    environment = dict(os.environ)
    environment["PATH"] = os.pathsep.join((str(system_root / "System32"), str(system_root)))
    environment["IMMUNE_ASSET_ROOT"] = str(run_dir / "missing-asset-root")
    report = {
        "schema": 1, "ok": False, "source_executable": str(source),
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "execution_directory": str(run_dir), "output_directory": str(output),
        "isolation": {"initial_files": ["IMMUNE.exe"], "cwd": str(run_dir),
                      "path": environment["PATH"], "asset_root": environment["IMMUNE_ASSET_ROOT"]},
        "checks": {}, "commands": {},
    }

    def command(name, arguments, timeout=120):
        started = time.monotonic()
        record = {"arguments": arguments}
        report["commands"][name] = record
        try:
            result = subprocess.run([str(executable), *arguments], cwd=run_dir, env=environment,
                                    capture_output=True, timeout=timeout, check=False)
            stdout, stderr = result.stdout, result.stderr
            record["returncode"] = result.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr = error.stdout or b"", error.stderr or b""
            record["timed_out"] = True
        record["seconds"] = round(time.monotonic() - started, 3)
        (output / f"{name}.stdout.txt").write_bytes(stdout)
        (output / f"{name}.stderr.txt").write_bytes(stderr)
        require(not record.get("timed_out"), f"{name} exceeded the {timeout}-second timeout")
        require(record["returncode"] == 0, f"{name} exited with {record['returncode']}")
        text = stdout.decode("utf-8", errors="replace")
        diagnostics = stderr.decode("utf-8", errors="replace")
        bad = BAD_LOG.search(diagnostics)
        require(bad is None, f"{name} reported an asset, shader, or runtime error: {bad.group(0) if bad else ''}")
        return text

    def check(name, callback):
        try:
            details = callback()
            report["checks"][name] = {"ok": True, **(details or {})}
        except (OSError, ValueError, struct.error, UnicodeError, zlib.error) as error:
            report["checks"][name] = {"ok": False, "error": str(error)}

    check("pe_imports", lambda: inspect_pe(executable, system_root))

    def help_check():
        text = command("help", ["--help"])
        require("USAGE" in text and "IMMUNE" in text and "--licenses" in text,
                "GUI executable did not provide its help through redirected stdout")
        return {"stdout_characters": len(text)}

    check("help", help_check)

    def licenses_check():
        text = command("licenses", ["--licenses"])
        require(all(marker in text for marker in ("sdl2", "Fredoka", "Nunito", "SIL OPEN FONT LICENSE")),
                "Bundled SDL/font notices are incomplete")
        return {"stdout_characters": len(text)}

    check("licenses", licenses_check)

    def interactive_check():
        # Startup --exec runs before the first menu layout, so a simulated
        # menu click is not a reliable way to close an interactive smoke test.
        # Observe startup, then terminate only the child created by this check.
        arguments = ["--threads", "1", "--width", "960", "--height", "540",
                     "--save", str(run_dir / "verification-save.json")]
        process = subprocess.Popen([str(executable), *arguments], cwd=run_dir, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        early_exit = False
        try:
            stdout, stderr = process.communicate(timeout=5)
            early_exit = True
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                stdout, stderr = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate()
        (output / "interactive.stdout.txt").write_bytes(stdout)
        (output / "interactive.stderr.txt").write_bytes(stderr)
        report["commands"]["interactive"] = {
            "arguments": arguments, "running_after_five_seconds": not early_exit,
            "test_child_terminated": not early_exit,
        }
        require(not early_exit, f"Interactive game exited during startup: {process.returncode}")
        diagnostics = stderr.decode("utf-8", errors="replace")
        discovered = re.search(r"discovered (\d+) level\(s\) in immune-embedded:/assets/levels", diagnostics)
        require(BAD_LOG.search(diagnostics) is None, "Interactive launch reported a runtime error")
        require("GL context created" in diagnostics and "tuning config loaded" in diagnostics
                and discovered is not None and int(discovered.group(1)) >= 10,
                "Interactive launch did not finish loading bundled graphics, tuning, and levels")
        return {"opened_and_running": True}

    check("interactive", interactive_check)

    def levels_check():
        result = json.loads(command("levels", ["--level-check", "immune-embedded:/assets/levels"]))
        levels = result.get("levels", [])
        campaigns = [level for level in levels
                     if PurePosixPath(level.get("path", "")).name.startswith("campaign_")]
        require(result.get("ok") is True and result.get("failed") == 0 and levels
                and all(level.get("ok") is True for level in levels),
                "Embedded level validation did not pass for every level")
        require(len(campaigns) >= 10, "Fewer than 10 bundled campaign levels were validated")
        return {"checked": len(levels), "campaign_levels": len(campaigns),
                "warnings": result.get("warnings", 0)}

    check("levels", levels_check)

    def screenshot_check(name, fixture, tick, towers=False, level=LEVEL):
        image = output / f"{name}.png"
        arguments = ["--screenshot", level, "--ui", "--threads", "1", "--width", "1280",
                     "--height", "720", "--tick", str(tick), "--out", str(image),
                     "--exec", fixture]
        if towers:
            arguments.append("--towers")
        metadata = json.loads(command(name, arguments))
        require(metadata.get("level") == level and metadata.get("tick") == tick,
                "Screenshot did not use the requested embedded campaign level and tick")
        return inspect_png(image, 1280, 720)

    check("menu", lambda: screenshot_check("menu", "ui screen menu", 0))
    check("tree", lambda: screenshot_check("tree", "ui tree sample; ui screen tree", 0))
    check("gameplay", lambda: screenshot_check("gameplay", "wave start", 240, towers=True))
    check("combat", lambda: screenshot_check("combat", "invuln on; spawn all 300", 120,
          towers=True, level="immune-embedded:/assets/levels/gym.json"))
    report["ok"] = all(item["ok"] for item in report["checks"].values())
    report_path = output / "verification.json"
    report["report_path"] = str(report_path)
    result = json.dumps(report, indent=2) + "\n"
    report_path.write_text(result, encoding="utf-8", newline="\n")
    print(result, end="")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
