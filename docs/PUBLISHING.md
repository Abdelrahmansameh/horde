# Publishing the standalone Windows game

[Documentation index](README.md) · [Building](BUILDING.md) · [Testing](TESTING.md)

The Windows publishing build produces one `IMMUNE.exe`. Players download and
run it directly, without an installer, adjacent DLLs, or an asset folder.
It targets Windows x64 and needs a graphics driver supporting OpenGL 4.5.
Progress is stored in the usual per-user application-data directory.

## Development stays the same

Continue using `tools\build.bat all windows-release` and editing the files in
`assets/`. Development builds use loose assets, hot reload, and the existing
shared dependencies. They never generate or compile the embedded asset pack.

`windows-standalone` is a separate CMake preset and build directory. It uses
Release optimization, the `x64-windows-static` vcpkg triplet, and the static
MSVC runtime. Its first build also compiles a separate set of dependencies;
subsequent builds reuse them.

## Make a publishing build

With the prerequisites in [Building](BUILDING.md), including Python 3.10 or newer:

```powershell
.\tools\publish.bat
```

The helper finds Visual Studio 2022 or reuses an x64 developer prompt, configures
and builds the standalone preset, runs CTest and the isolated executable check,
then copies the game to:

```text
%LOCALAPPDATA%\horde-build\windows-standalone\publish\IMMUNE.exe
```

Pass an output directory to choose a different destination. `VCPKG_ROOT`
defaults to `C:\dev\vcpkg`; set it when vcpkg is installed elsewhere.

Direct commands in an x64 developer prompt are:

```text
cmake --preset windows-standalone
cmake --build --preset windows-standalone
ctest --preset windows-standalone
python tools/verify_standalone.py "%LOCALAPPDATA%\horde-build\windows-standalone\bin\immune.exe"
```

## Embedded assets and licenses

The publishing build runs [embed_assets.py](../tools/embed_assets.py) to index
the runtime assets and generate Windows RCDATA resources. The game reads their
mapped bytes directly from its executable; it does not unpack an asset folder
at startup. New, changed, or removed assets update the resource build. Backup
files such as `*.bak` are excluded.

[FileIO](../src/platform/FileIO.h) provides the same reads and directory listings
for both modes. In the standalone build, `asset_path()` returns read-only
`immune-embedded:/assets/...` paths. `IMMUNE_ASSET_ROOT` does not override bundled
assets. Explicit external level, config, save, and output paths still use the
normal filesystem. Use the development build to edit shipped assets, or save
an edited level to a separate external file.

[collect_notices.py](../tools/collect_notices.py) collects the installed runtime
dependencies' copyrights and the checked-in font license. All are embedded,
including `assets/fonts/OFL.txt`. To print the combined notices:

```powershell
.\IMMUNE.exe --licenses | Set-Content -Encoding utf8 .\THIRD_PARTY_NOTICES.txt
```

## Verify and publish

[verify_standalone.py](../tools/verify_standalone.py) copies only the executable
into a fresh directory outside the checkout, clears development asset lookup,
and restricts PATH to Windows system directories. It checks executable imports,
licenses, a real game launch, bundled levels, and rendered menu,
tree, and gameplay captures. The
report and captures remain available for inspection. The launch check closes
only the test process it created after observing successful startup.
This checks isolation on
the development machine; it is not a substitute for broader hardware testing.

Before uploading, inspect the captures and review any failing tests. Publish
`IMMUNE.exe` as a GitHub Release attachment with a version tag, Windows/OpenGL
requirements, and a SHA-256 checksum. Players should download this attachment
from the release's Assets list. GitHub's automatic source archives contain
the repository, rather than the playable game.

When releasing a working snapshot with local gameplay changes, record those
files in the release tag's source snapshot. Keep unrelated local files and
the user's active development checkout intact. Verify the uploaded asset's
checksum against the tested executable.
