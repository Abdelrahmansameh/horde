# Globbed so that parallel module agents can add test_<module>.cpp files without
# all contending for an edit to this one file. CONFIGURE_DEPENDS makes the build
# re-glob when a new test file appears.
file(GLOB IMMUNE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(immune_tests ${IMMUNE_TEST_SOURCES})

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
