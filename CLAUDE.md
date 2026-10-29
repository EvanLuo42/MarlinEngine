# Marlin Engine

AAA game engine (PC + consoles). Design decisions live in `Documentation/Architecture/` — read the relevant file before designing anything, and ask before deviating from it.

## Version control

- Epic **Lore**, not git. The repo is local-only (`remote_url = ""`, `identity = "EvanLuo42"` in `.lore/config.toml`).
- `lore status --scan`, `lore stage --scan <paths>`, `lore commit "<msg>"`. Commit only when asked.
- Messages: short Conventional Commits (`feat(toolkit): …`, `docs(architecture): …`) with a brief bullet body.
- `.loreignore` is gitignore syntax; `!` re-includes must be rooted (`!/Source/ThirdParty/`).

## Layout

- `Source/{Runtime,Developer,Editor,Programs,ThirdParty}` — module kind follows the top folder.
- `Platforms/<Layer>/` platform layers, `Extras/<Tool>/` checked-in tools, binaries in `Extras/<Tool>/<Platform>/<Arch>/` (found via `EnginePaths.tool_executable`). Each tool (`<Tool>.tool.py`) and third-party module declares `version`, `url` and `license` in its rules file, never in folder names, and keeps the upstream `LICENSE` text beside it.
- `Samples/<Name>/` sample projects; `Samples/Sandbox` is what Editor and Game open when started from Visual Studio (`solution.debug_project`).
- `Build/` settings and presets; `Intermediate/`, `Binaries/`, `Saved/` are generated.
- Ask before changing the directory layout.

## Build (MarlinToolkit, Python)

- `marlin generate | build | test | clean | doc` (`.venv/Scripts/marlin.exe`). Rules files: `*.module.py`, `*.target.py`, `*.tool.py` (API in `marlin/rules.py`).
- Settings: `Build/BuildSettings.toml` < `.local.toml` < `--preset` < `--set`.
- LLVM clang only (never MSVC cl); no cross-platform or cross-arch builds; consoles are always monolithic.
- A module's `Tests/` folder becomes `<Module>Tests`; targets with `test_modules = True` link them.
- Python 3.14; keep `ruff format`, `ruff check` (line length 120) and `ty check` clean.

## C++ rules

- C++20, no modules/coroutines, no exceptions, no RTTI (Program-only modules may opt out).
- Runtime uses no std headers beyond language support (`<initializer_list>`, `<new>`, `<cstddef>`, `<cstdint>`, `<compare>`); prefer clang builtins.
- No `WITH_EDITOR`-style macros: modules are shared by Editor/Game/Server/Programs; differences come from which modules are linked.
- Lifecycle belongs to Subsystems, not modules. No work in static constructors.
- C# naming: `PascalCase` types/methods/public fields/files, `camelCase` locals/params, `_camelCase` private fields, `I` interfaces, `T` template params; namespace `Marlin`.
- Reflection is codegen-based (`MCLASS`/`MPROPERTY`, generated `.gen.h/.gen.cpp`).
- Avoid hard-coded mode enums for extensible concepts; prefer data-driven, open registration.
