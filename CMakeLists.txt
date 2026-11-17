cmake_minimum_required(VERSION 3.24)
project(myulk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MYULK_BUILD_TESTS "Build unit tests" ON)

# Windows: the C/C++ runtime is linked into the program (and SDL), so it runs
# on a PC without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
    src/adventure/engine.cpp
    src/adventure/scenes.cpp
)
target_include_directories(myulk_core PUBLIC src)
target_link_libraries(myulk_core PUBLIC nlohmann_json::nlohmann_json)

# --- game executable ----------------------------------------------------------
add_executable(myulk
    src/app/main.cpp
    src/app/assets.cpp
    src/app/form_view.cpp
    src/app/audio.cpp
    src/app/cursors.cpp
    src/app/adventure_screen.cpp
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
    # Packs the extracted pictures and the adventure sounds into myulk.data
    # (~170 MB). Fine for local testing; per-chapter loading comes later.
    set(MYULK_EXTRACTED_DIR "${CMAKE_SOURCE_DIR}/extracted" CACHE PATH
        "Extracted game data to package for the web build")
    set_target_properties(myulk PROPERTIES SUFFIX ".html")
    target_link_options(myulk PRIVATE
        "--preload-file=${MYULK_EXTRACTED_DIR}/resources@/data/resources"
        "--preload-file=${MYULK_EXTRACTED_DIR}/iso/SOUNDS@/data/iso/SOUNDS"
        "--shell-file=${CMAKE_SOURCE_DIR}/web/shell.html"
        "-sALLOW_MEMORY_GROWTH=1")
    set_property(TARGET myulk APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/shell.html")
endif()

# --- recompiled game ------------------------------------------------------------
# tools/recomp/recomp.py turns the user's MYULK.EXE into C++ at build time; the
# output is derived from the original binary and stays in the build folder.
set(MYULK_EXE "${CMAKE_SOURCE_DIR}/extracted/iso/MYULK/MYULK.EXE" CACHE FILEPATH
    "Original MYULK.EXE to recompile")
if(EXISTS "${MYULK_EXE}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
    set(GEN_SOURCES "${GEN_DIR}/dispatch.cpp" "${GEN_DIR}/imports.cpp")
    foreach(n RANGE 1 33)
        string(LENGTH "${n}" len)
        if(len EQUAL 1)
            set(n "0${n}")
        endif()
        list(APPEND GEN_SOURCES "${GEN_DIR}/seg${n}.cpp")
    endforeach()
    file(GLOB RECOMP_TOOL "${CMAKE_SOURCE_DIR}/tools/recomp/*.py" "${CMAKE_SOURCE_DIR}/tools/ne.py")
    add_custom_command(
        OUTPUT ${GEN_SOURCES} "${GEN_DIR}/funcs.hpp" "${GEN_DIR}/import_ids.hpp"
        COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/tools/recomp/recomp.py"
                "${MYULK_EXE}" "${GEN_DIR}"
                --extra-entries "${CMAKE_SOURCE_DIR}/tools/recomp/extra_entries.txt"
        DEPENDS ${RECOMP_TOOL} "${MYULK_EXE}" "${CMAKE_SOURCE_DIR}/tools/recomp/extra_entries.txt"
                "${CMAKE_SOURCE_DIR}/tools/recomp/hooks.txt"
                "${CMAKE_SOURCE_DIR}/analysis/imports.csv"
        COMMENT "Recompiling MYULK.EXE"
        VERBATIM)

    # Text: stb_truetype (public domain) renders the Liberation fonts (SIL Open
    # Font License; metric-compatible with Arial, Times New Roman, Courier New),
    # which are embedded into the program.
    FetchContent_Declare(stb
        URL https://github.com/nothings/stb/archive/9600522de53973336fa033a289858a4354cbb04c.tar.gz
        URL_HASH SHA256=8546b1281cf61e4b5229e9eb05055d765f1b076f6ba03aab377df09a20fb613b
        SOURCE_SUBDIR none)
    FetchContent_Declare(liberation_fonts
        URL https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz
        URL_HASH SHA256=7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0
        SOURCE_SUBDIR none)
    FetchContent_MakeAvailable(stb liberation_fonts)
    set(FONTS_CPP "${CMAKE_BINARY_DIR}/fonts_embedded.cpp")
    set(FONT_FILES
        "sans=${liberation_fonts_SOURCE_DIR}/LiberationSans-Regular.ttf"
        "sans-bold=${liberation_fonts_SOURCE_DIR}/LiberationSans-Bold.ttf"
        "serif=${liberation_fonts_SOURCE_DIR}/LiberationSerif-Regular.ttf"
        "serif-bold=${liberation_fonts_SOURCE_DIR}/LiberationSerif-Bold.ttf"
        "mono=${liberation_fonts_SOURCE_DIR}/LiberationMono-Regular.ttf")
    add_custom_command(
        OUTPUT "${FONTS_CPP}"
        COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/tools/embed_fonts.py" "${FONTS_CPP}" ${FONT_FILES}
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/embed_fonts.py"
        COMMENT "Embedding fonts"
        VERBATIM)

    file(GLOB WIN16_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/recomp/win16/*.cpp"
         "${CMAKE_SOURCE_DIR}/src/recomp/enhance/*.cpp")
    add_executable(myulk_recomp
        src/recomp/main.cpp
        src/recomp/runtime/cpu.cpp
        src/recomp/runtime/runtime.cpp
        ${WIN16_SOURCES}
        ${GEN_SOURCES}
        "${FONTS_CPP}")
    target_include_directories(myulk_recomp PRIVATE src "${GEN_DIR}" "${stb_SOURCE_DIR}")
    set_source_files_properties("${FONTS_CPP}" PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
    target_link_libraries(myulk_recomp PRIVATE SDL3::SDL3-static)
    if(MSVC)
        target_compile_options(myulk_recomp PRIVATE /utf-8 /bigobj)
        # a Windows program, no console window (stderr still works when redirected)
        target_link_options(myulk_recomp PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
        # addresses in hang reports (hang.txt) are looked up in myulk_recomp.map
        target_link_options(myulk_recomp PRIVATE /MAP)
        # generated code: no warnings, moderate optimisation keeps compile times sane
        set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0;/O1")
    else()
        set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-O1")
    endif()

    if(EMSCRIPTEN)
        # The game's message loop blocks (GetMessage, ShowModal, MCI_WAIT); JSPI
        # lets those waits yield to the browser without instrumenting the code.
        # Delphi exceptions use C++ exceptions, so native wasm exceptions too.
        target_compile_options(myulk_recomp PRIVATE -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=0)
        set_target_properties(myulk_recomp PROPERTIES SUFFIX ".html")
        # The user's extracted CD is copied next to the page (game/) with a
        # manifest; the page fetches each file the first time the game opens it
        # (src/recomp/win16/webdata.cpp). It appears at /game (C:\ in the game).
        add_custom_target(myulk_web_data
            COMMAND Python3::Interpreter -I "${CMAKE_SOURCE_DIR}/tools/web_data.py"
                    "${MYULK_EXTRACTED_DIR}/iso" "${CMAKE_BINARY_DIR}/game"
            COMMENT "Copying game data for the web build"
            VERBATIM)
        add_dependencies(myulk_recomp myulk_web_data)
        target_link_options(myulk_recomp PRIVATE
            -fwasm-exceptions
            -sWASM_LEGACY_EXCEPTIONS=0
            -sJSPI
            -sALLOW_MEMORY_GROWTH=1
            -sINITIAL_MEMORY=128MB
            -sSTACK_SIZE=2MB
            -sINVOKE_RUN=0
            -sEXPORTED_RUNTIME_METHODS=callMain
            -lidbfs.js
            -sFORCE_FILESYSTEM=1
            -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_myulk_open_settings,_myulk_show_hints
            "--shell-file=${CMAKE_SOURCE_DIR}/web/recomp_shell.html")
        set_property(TARGET myulk_recomp APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/recomp_shell.html")
    endif()
endif()

# --- tests --------------------------------------------------------------------
if(MYULK_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_executable(myulk_tests tests/test_main.cpp tests/test_calendar.cpp tests/test_form.cpp
        tests/test_adventure.cpp)
    target_link_libraries(myulk_tests PRIVATE myulk_core)
    add_test(NAME myulk_tests COMMAND myulk_tests)
endif()
