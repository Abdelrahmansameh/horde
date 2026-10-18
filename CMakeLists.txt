cmake_minimum_required(VERSION 3.21)
project(immune LANGUAGES CXX)

option(IMMUNE_STANDALONE "Embed assets and statically link the Windows publishing build" OFF)
set(IMMUNE_THIRD_PARTY_NOTICES "" CACHE FILEPATH
  "Optional third-party notice text to embed instead of collecting dependency licenses")
if(IMMUNE_STANDALONE)
  if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "IMMUNE_STANDALONE currently requires Windows and MSVC")
  endif()
  if(NOT VCPKG_TARGET_TRIPLET STREQUAL "x64-windows-static")
    message(FATAL_ERROR "IMMUNE_STANDALONE requires the x64-windows-static vcpkg triplet")
  endif()
  # Keep this separate from development's /MD runtime and shared dependencies.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:preprocessor /utf-8 /MP)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  # Release-family builds: full optimization + fast (but not unsafe) FP.
  add_compile_options($<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
  add_compile_options(-Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Dependencies (vcpkg manifest mode)
# ---------------------------------------------------------------------------
find_package(SDL2 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(Catch2 3 CONFIG REQUIRED)
# nanosvg (vcpkg port `nanosvg`) is used header-only: its implementation is
# compiled once in src/gui/icons/NanoSvgImpl.cpp, so only the include path is
# needed and the port's own library targets are not linked.
find_path(NANOSVG_INCLUDE_DIR nanosvg.h PATH_SUFFIXES nanosvg REQUIRED)

if(IMMUNE_STANDALONE)
  find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
  set(IMMUNE_EMBEDDED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_assets")
  file(MAKE_DIRECTORY "${IMMUNE_EMBEDDED_DIR}")
  if(NOT IMMUNE_THIRD_PARTY_NOTICES)
    set(IMMUNE_THIRD_PARTY_NOTICES "${IMMUNE_EMBEDDED_DIR}/THIRD_PARTY_NOTICES.txt")
    execute_process(
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/collect_notices.py"
        --vcpkg-root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
        --fonts "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL.txt"
        --output "${IMMUNE_THIRD_PARTY_NOTICES}"
      RESULT_VARIABLE IMMUNE_NOTICE_RESULT)
    if(NOT IMMUNE_NOTICE_RESULT EQUAL 0)
      message(FATAL_ERROR "Could not collect third-party notices for the standalone executable")
    endif()
  endif()
  file(GLOB_RECURSE IMMUNE_EMBEDDED_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
  set(IMMUNE_EMBEDDED_HEADER "${IMMUNE_EMBEDDED_DIR}/EmbeddedAssetIndex.h")
  set(IMMUNE_EMBEDDED_RESOURCE "${IMMUNE_EMBEDDED_DIR}/immune_assets.rc")
  add_custom_command(
    OUTPUT "${IMMUNE_EMBEDDED_HEADER}" "${IMMUNE_EMBEDDED_RESOURCE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_assets.py"
      --root "${CMAKE_CURRENT_SOURCE_DIR}" --output "${IMMUNE_EMBEDDED_DIR}"
      --notices "${IMMUNE_THIRD_PARTY_NOTICES}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_assets.py"
      "${IMMUNE_THIRD_PARTY_NOTICES}" ${IMMUNE_EMBEDDED_INPUTS}
    COMMENT "Embedding game assets and third-party notices"
    VERBATIM)
  add_custom_target(immune_embedded_assets
    DEPENDS "${IMMUNE_EMBEDDED_HEADER}" "${IMMUNE_EMBEDDED_RESOURCE}")
  # RC does not generate an asset depfile; rebuild its object when bytes change.
  set_source_files_properties("${IMMUNE_EMBEDDED_RESOURCE}" PROPERTIES
    GENERATED TRUE
    OBJECT_DEPENDS "${IMMUNE_EMBEDDED_INPUTS};${IMMUNE_THIRD_PARTY_NOTICES}")
endif()

# ---------------------------------------------------------------------------
# Shared settings target — every module links this for include paths and flags.
# ---------------------------------------------------------------------------
add_library(immune_common INTERFACE)
target_include_directories(immune_common INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${Stb_INCLUDE_DIR}")
target_link_libraries(immune_common INTERFACE glm::glm)
target_compile_features(immune_common INTERFACE cxx_std_20)

# ---------------------------------------------------------------------------
# One library target per module (docs/AGENT_BRIEF.md: agents own directories).
# ---------------------------------------------------------------------------

add_library(immune_core STATIC
  src/core/Arena.cpp
  src/core/Clock.cpp
  src/core/JobSystem.cpp
  src/core/Log.cpp
  src/core/Profiler.cpp
  src/core/Rng.cpp)
target_link_libraries(immune_core PUBLIC immune_common)

add_library(immune_platform STATIC
  src/platform/EmbeddedAssets.cpp
  src/platform/FileIO.cpp
  src/platform/Input.cpp
  src/platform/Window.cpp)
if(IMMUNE_STANDALONE)
  set(IMMUNE_SDL2_TARGET SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
  set(IMMUNE_SDL2_TARGET SDL2::SDL2)
else()
  set(IMMUNE_SDL2_TARGET SDL2::SDL2-static)
endif()
target_link_libraries(immune_platform PUBLIC immune_core glad::glad ${IMMUNE_SDL2_TARGET})
if(IMMUNE_STANDALONE)
  target_compile_definitions(immune_platform PUBLIC IMMUNE_EMBEDDED_ASSETS)
  target_include_directories(immune_platform PRIVATE "${IMMUNE_EMBEDDED_DIR}")
  add_dependencies(immune_platform immune_embedded_assets)
endif()

# Tuning config. Sits above platform and below sim so that sim never needs a
# JSON dependency: the config produces plain structs, and game/ + app/ inject
# them into SimDesc/ChaffTuning at construction time.
add_library(immune_config STATIC
  src/config/ConfigStore.cpp
  src/config/Field.cpp
  src/config/Json.cpp
  src/config/Registry.cpp)
target_link_libraries(immune_config PUBLIC immune_core immune_platform
  nlohmann_json::nlohmann_json)

add_library(immune_sim STATIC
  src/sim/SimWorld.cpp
  src/sim/burrow/Burrow.cpp
  src/sim/chaff/ChaffBuffers.cpp
  src/sim/chaff/ChaffSystem.cpp
  src/sim/chaff/HitFlash.cpp
  src/sim/chaff/ReplicationSplit.cpp
  src/sim/damage/DamageField.cpp
  src/sim/ecs/AiStateMachine.cpp
  src/sim/ecs/EcsWorld.cpp
  src/sim/ecs/NamedAgents.cpp
  src/sim/flowfield/FlowField.cpp
  src/sim/fluid/Fluid.cpp
  src/sim/hostile/HostileAttacks.cpp
  src/sim/projectile/Projectiles.cpp
  src/sim/scar/Scars.cpp
  src/sim/swarm/Swarmers.cpp
  src/sim/spatial/SpatialHash.cpp
  src/sim/squad/Squads.cpp)
target_link_libraries(immune_sim PUBLIC immune_core EnTT::EnTT)

add_library(immune_render STATIC
  src/render/Camera.cpp
  src/render/Renderer.cpp
  src/render/Screenshot.cpp
  src/render/Shader.cpp)
target_link_libraries(immune_render PUBLIC immune_core immune_platform immune_sim immune_vfx)

# Cosmetic particle layer. Deliberately NOT part of immune_sim: it has its own
# RNG, runs on the render clock, and must never contribute to state_hash().
add_library(immune_vfx STATIC src/vfx/DeathVfx.cpp src/vfx/Particles.cpp)
target_link_libraries(immune_vfx PUBLIC immune_core immune_sim)

add_library(immune_game STATIC
  src/game/abilities/AbilityConfigApply.cpp
  src/game/abilities/ActiveAbilities.cpp
  src/game/autoplay/AutoPlayer.cpp
  src/game/config/EnemyConfig.cpp
  src/game/config/GameConfig.cpp
  src/game/config/TowerConfig.cpp
  src/game/economy/Economy.cpp
  src/game/editor/LevelDoc.cpp
  src/game/editor/LevelTemplates.cpp
  src/game/editor/LevelValidate.cpp
  src/game/enemies/EnemyRoster.cpp
  src/game/gym/GymCommands.cpp
  src/game/level/Level.cpp
  src/game/level/LevelWriter.cpp
  src/game/level/RenderSdf.cpp
  src/game/meta/ImmunityTree.cpp
  src/game/meta/MetaProgression.cpp
  src/game/session/LevelSession.cpp
  src/game/telemetry/RunTelemetry.cpp
  src/game/towers/TowerSystem.cpp
  src/game/wave/WaveDirector.cpp)
target_link_libraries(immune_game PUBLIC immune_core immune_config immune_sim immune_render
  nlohmann_json::nlohmann_json)

# The UI framework (docs/ui-concepts, src/gui): SDF shapes, text, icons, and
# the GL backend. Game-agnostic: it never includes game/ or sim/.
add_library(immune_gui STATIC
  src/gui/Color.cpp
  src/gui/backend/GlBackend.cpp
  src/gui/core/Gui.cpp
  src/gui/core/Widget.cpp
  src/gui/draw/DrawList.cpp
  src/gui/draw/Shape.cpp
  src/gui/icons/IconLibrary.cpp
  src/gui/icons/NanoSvgImpl.cpp
  src/gui/style/Theme.cpp
  src/gui/text/Font.cpp
  src/gui/text/Text.cpp
  src/gui/widgets/Widgets.cpp)
target_include_directories(immune_gui PRIVATE "${NANOSVG_INCLUDE_DIR}")
target_link_libraries(immune_gui PUBLIC immune_core immune_platform immune_render
  PRIVATE nlohmann_json::nlohmann_json)
# Third-party C compiled at this project's warning level would drown the build.
set_source_files_properties(src/gui/icons/NanoSvgImpl.cpp PROPERTIES
  COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")

add_library(immune_ui STATIC src/ui/DevUi.cpp src/ui/Fonts.cpp
  src/ui/GymPanel.cpp
  src/ui/hud/HudParts.cpp
  src/ui/hud/HudScreen.cpp
  src/ui/front/Backdrop.cpp src/ui/front/LevelCell.cpp src/ui/front/FrontEnd.cpp
  src/ui/front/TreeScreen.cpp
  src/ui/front/VirusSwipe.cpp src/ui/front/VirusSwipeConfig.cpp
  src/ui/editor/EditorCanvas.cpp
  src/ui/editor/EditorGizmos.cpp
  src/ui/editor/EditorPanels.cpp)
target_link_libraries(immune_ui PUBLIC immune_core immune_platform immune_render immune_game immune_gui
  PRIVATE imgui::imgui)

add_library(immune_audio STATIC src/audio/Audio.cpp)
target_link_libraries(immune_audio PUBLIC immune_core PRIVATE ${IMMUNE_SDL2_TARGET})

add_library(immune_app STATIC
  src/app/App.cpp
  src/app/AutoplayMode.cpp
  src/app/Cli.cpp
  src/app/EditorMode.cpp
  src/app/GameState.cpp
  src/app/LevelTools.cpp
  src/app/Modes.cpp
  src/app/UiBridge.cpp)
target_link_libraries(immune_app PUBLIC
  immune_core immune_platform immune_sim immune_render immune_game immune_ui immune_audio
  nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(immune src/app/main.cpp)
target_link_libraries(immune PRIVATE immune_app)

if(IMMUNE_STANDALONE)
  target_sources(immune PRIVATE "${IMMUNE_EMBEDDED_RESOURCE}")
  add_dependencies(immune immune_embedded_assets)
  # A normal double-click opens only the game. Retain main() and inherited
  # redirected stdout/stderr for CLI checks and support diagnostics.
  set_target_properties(immune PROPERTIES WIN32_EXECUTABLE TRUE)
  target_link_options(immune PRIVATE /ENTRY:mainCRTStartup)
endif()

# SDL2 on Windows brings its own main(); we provide our own console entry point.
if(NOT IMMUNE_STANDALONE AND TARGET SDL2::SDL2main)
  target_link_libraries(immune PRIVATE SDL2::SDL2main)
endif()

# Copy runtime DLLs next to the executable so the binary runs from anywhere.
if(WIN32 AND NOT IMMUNE_STANDALONE)
  add_custom_command(TARGET immune POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:immune> $<TARGET_FILE_DIR:immune>
    COMMAND_EXPAND_LISTS)
endif()

# The executable resolves assets relative to itself; point it at the source tree.
set_target_properties(immune PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
if(IMMUNE_STANDALONE)
  target_sources(immune_tests PRIVATE "${IMMUNE_EMBEDDED_RESOURCE}")
  add_dependencies(immune_tests immune_embedded_assets)
endif()
