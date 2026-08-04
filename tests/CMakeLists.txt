add_executable(immune_tests
  test_main.cpp
  test_rng.cpp
  test_arena.cpp
  test_clock.cpp
  test_jobsystem.cpp
  test_profiler.cpp
  test_chaff_soa.cpp
  test_camera.cpp
  test_cli.cpp
  test_ecs_scheduler.cpp)

target_link_libraries(immune_tests PRIVATE
  immune_app
  Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(immune_tests
  DISCOVERY_MODE PRE_TEST
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(WIN32)
  add_custom_command(TARGET immune_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:immune_tests> $<TARGET_FILE_DIR:immune_tests>
    COMMAND_EXPAND_LISTS)
endif()
