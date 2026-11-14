cmake_minimum_required(VERSION 3.24)
project(myulk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MYULK_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- SDL3 -------------------------------------------------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.8
    GIT_SHALLOW TRUE)

# --- nlohmann/json ----------------------------------------------------------
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)

FetchContent_MakeAvailable(SDL3 json)

# --- core library (no SDL: game logic, form model) ----------------------------
add_library(myulk_core STATIC
    src/core/calendar.cpp
    src/core/form.cpp
)
target_include_directories(myulk_core PUBLIC src)
target_link_libraries(myulk_core PUBLIC nlohmann_json::nlohmann_json)

# --- game executable ----------------------------------------------------------
add_executable(myulk
    src/app/main.cpp
    src/app/assets.cpp
    src/app/form_view.cpp
)
target_link_libraries(myulk PRIVATE myulk_core SDL3::SDL3-static)

if(MSVC)
    target_compile_options(myulk_core PRIVATE /W4 /utf-8)
    target_compile_options(myulk PRIVATE /W4 /utf-8)
else()
    target_compile_options(myulk_core PRIVATE -Wall -Wextra)
    target_compile_options(myulk PRIVATE -Wall -Wextra)
endif()

if(EMSCRIPTEN)
    # Game data is the user's own extracted copy, preloaded into the virtual FS.
    # Only the main EXE's resources (forms and their images) for now; the
    # scene art in PICTDLL is large and will be loaded on demand later.
    set(MYULK_WEB_DATA "${CMAKE_SOURCE_DIR}/extracted/resources/MYULK" CACHE PATH
        "Extracted MYULK resources to package for the web build")
    set_target_properties(myulk PROPERTIES SUFFIX ".html")
    target_link_options(myulk PRIVATE
        "--preload-file=${MYULK_WEB_DATA}@/data/resources/MYULK"
        "--shell-file=${CMAKE_SOURCE_DIR}/web/shell.html"
        "-sALLOW_MEMORY_GROWTH=1")
    set_property(TARGET myulk APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/shell.html")
endif()

# --- tests --------------------------------------------------------------------
if(MYULK_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_executable(myulk_tests tests/test_main.cpp tests/test_calendar.cpp tests/test_form.cpp)
    target_link_libraries(myulk_tests PRIVATE myulk_core)
    add_test(NAME myulk_tests COMMAND myulk_tests)
endif()
