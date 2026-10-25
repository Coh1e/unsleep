cmake_minimum_required(VERSION 3.28)
project(Unsleep VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "Unsleep requires Windows.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(unsleep_core src/core.cpp src/settings.cpp)
target_include_directories(unsleep_core PUBLIC include)

# The icon resources. Listed explicitly so replacing an .ico is a real build
# dependency: without this, editing an icon leaves a stale .rc.res and the link
# has "no work to do" even though the binary still carries the old picture. The
# paths are absolute because the OBJC_DEPENDS edge is resolved from the build
# directory, not the source directory.
set(UNSLEEP_ICON_NAMES
  app.ico
  light-inactive.ico
  light-active.ico
  light-screen.ico
  light-screen-paused.ico
  light-duration.ico
  light-screen-duration.ico
  light-screen-paused-duration.ico
  dark-inactive.ico
  dark-active.ico
  dark-screen.ico
  dark-screen-paused.ico
  dark-duration.ico
  dark-screen-duration.ico
  dark-screen-paused-duration.ico
)
set(UNSLEEP_ICONS)
foreach(name ${UNSLEEP_ICON_NAMES})
  list(APPEND UNSLEEP_ICONS "${CMAKE_CURRENT_SOURCE_DIR}/resources/${name}")
endforeach()
set(UNSLEEP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest")

# The icon artwork the .ico files are converted from, plus the converter itself.
# They are not compiled, but a build that used them should notice when they
# change: rebuilding the .ico by hand and forgetting to reconfigure is exactly
# how stale pictures shipped before. They are listed as dependencies of the
# resource step for the same reason as the .ico files.
set(UNSLEEP_ICON_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/source/gpt-cup-refinement-002.png"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/generate-icons-gpt.mjs"
)

add_executable(unsleep WIN32 src/win32.cpp resources/unsleep.rc)
# The .rc includes the icons by file name, so CMake cannot see the edges on its
# own; declare them explicitly. The icons stay out of the source list (the .rc
# embeds them); they are marked as header-only inputs of the resource step.
# OBJECT_DEPENDS is what makes `cmake --build` actually recompile the resources
# when a picture changes; without it Ninja reports "no work to do" and the exe
# keeps the old icons.
set_source_files_properties(${UNSLEEP_ICONS} ${UNSLEEP_MANIFEST} PROPERTIES HEADER_FILE_ONLY TRUE)
set_property(SOURCE resources/unsleep.rc APPEND PROPERTY OBJECT_DEPENDS "${UNSLEEP_ICONS};${UNSLEEP_MANIFEST}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${UNSLEEP_ICON_SOURCES})
target_compile_definitions(unsleep PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_link_libraries(unsleep PRIVATE unsleep_core user32 shell32 advapi32 wtsapi32)

include(CTest)
if(BUILD_TESTING)
  add_executable(unsleep_core_tests tests/core_tests.cpp)
  target_link_libraries(unsleep_core_tests PRIVATE unsleep_core)
  # Settings file tests only ever write inside this project-local test area.
  # The path is deliberately stable (not tied to a single task id) so a fresh
  # clone can build and test without depending on task history.
  target_compile_definitions(unsleep_core_tests PRIVATE
    "UNSLEEP_TEST_TMP_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/.pi/tmp/core-tests\"")
  add_test(NAME unsleep_core COMMAND unsleep_core_tests)
endif()

if(MSVC)
  foreach(target unsleep_core unsleep unsleep_core_tests)
    if(TARGET ${target})
      target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/permissive-;/sdl;/utf-8;/guard:cf>")
    endif()
  endforeach()
  target_link_options(unsleep PRIVATE /DYNAMICBASE /NXCOMPAT /GUARD:CF /MANIFEST:NO)
endif()
