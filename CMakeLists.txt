cmake_minimum_required(VERSION 3.21)
project(immune LANGUAGES CXX)

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
  src/platform/FileIO.cpp
  src/platform/Input.cpp
  src/platform/Window.cpp)
if(TARGET SDL2::SDL2)
  set(IMMUNE_SDL2_TARGET SDL2::SDL2)
else()
  set(IMMUNE_SDL2_TARGET SDL2::SDL2-static)
endif()
target_link_libraries(immune_platform PUBLIC immune_core glad::glad ${IMMUNE_SDL2_TARGET})

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
  src/sim/chaff/ChaffBuffers.cpp
  src/sim/chaff/ChaffSystem.cpp
  src/sim/damage/DamageField.cpp
  src/sim/ecs/AiStateMachine.cpp
  src/sim/ecs/EcsWorld.cpp
  src/sim/ecs/NamedAgents.cpp
  src/sim/flowfield/FlowField.cpp
  src/sim/fluid/Fluid.cpp
  src/sim/projectile/Projectiles.cpp
  src/sim/swarm/Swarmers.cpp
  src/sim/spatial/SpatialHash.cpp)
target_link_libraries(immune_sim PUBLIC immune_core EnTT::EnTT)

add_library(immune_render STATIC
  src/render/Camera.cpp
  src/render/Renderer.cpp
  src/render/Screenshot.cpp
  src/render/Shader.cpp)
target_link_libraries(immune_render PUBLIC immune_core immune_platform immune_sim immune_vfx)

# Cosmetic particle layer. Deliberately NOT part of immune_sim: it has its own
# RNG, runs on the render clock, and must never contribute to state_hash().
add_library(immune_vfx STATIC src/vfx/Particles.cpp)
target_link_libraries(immune_vfx PUBLIC immune_core immune_sim)

add_library(immune_game STATIC
  src/game/abilities/AbilityConfigApply.cpp
  src/game/abilities/ActiveAbilities.cpp
  src/game/config/EnemyWaveConfig.cpp
  src/game/config/GameConfig.cpp
  src/game/config/TowerConfig.cpp
  src/game/economy/Economy.cpp
  src/game/enemies/EnemyRoster.cpp
  src/game/gym/GymCommands.cpp
  src/game/level/Level.cpp
  src/game/meta/MetaProgression.cpp
  src/game/towers/TowerSystem.cpp
  src/game/wave/WaveDirector.cpp)
target_link_libraries(immune_game PUBLIC immune_core immune_config immune_sim immune_render
  nlohmann_json::nlohmann_json)

add_library(immune_ui STATIC src/ui/Hud.cpp src/ui/Menu.cpp src/ui/Fonts.cpp
  src/ui/GymPanel.cpp)
target_link_libraries(immune_ui PUBLIC immune_core immune_platform immune_render immune_game
  PRIVATE imgui::imgui)

add_library(immune_audio STATIC src/audio/Audio.cpp)
target_link_libraries(immune_audio PUBLIC immune_core PRIVATE ${IMMUNE_SDL2_TARGET})

add_library(immune_app STATIC
  src/app/App.cpp
  src/app/Cli.cpp
  src/app/GameState.cpp
  src/app/Modes.cpp)
target_link_libraries(immune_app PUBLIC
  immune_core immune_platform immune_sim immune_render immune_game immune_ui immune_audio
  nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(immune src/app/main.cpp)
target_link_libraries(immune PRIVATE immune_app)

# SDL2 on Windows brings its own main(); we provide our own console entry point.
if(TARGET SDL2::SDL2main)
  target_link_libraries(immune PRIVATE SDL2::SDL2main)
endif()

# Copy runtime DLLs next to the executable so the binary runs from anywhere.
if(WIN32)
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
