cmake_minimum_required(VERSION 3.25)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

# obtain revision info from git
find_package(Git)
if (GIT_FOUND)
    # make sure version information gets re-run when the current Git HEAD changes
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} rev-parse --git-path HEAD
            OUTPUT_VARIABLE partyboard_git_head_filename
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${partyboard_git_head_filename}")

    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} rev-parse --symbolic-full-name HEAD
            OUTPUT_VARIABLE partyboard_git_head_symbolic
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMAND ${GIT_EXECUTABLE} rev-parse --git-path ${partyboard_git_head_symbolic}
            OUTPUT_VARIABLE partyboard_git_head_symbolic_filename
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${partyboard_git_head_symbolic_filename}")

    # defines PARTY_BOARD_WC_REVISION
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
            OUTPUT_VARIABLE PARTY_BOARD_WC_REVISION
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    # defines PARTY_BOARD_WC_DESCRIBE
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} describe --tags --long --dirty --match "v*"
            OUTPUT_VARIABLE PARTY_BOARD_WC_DESCRIBE
            OUTPUT_STRIP_TRAILING_WHITESPACE)

    # remove the git hash, then collapse a clean "-0" suffix only
    string(REGEX REPLACE "-[^-]+(-dirty|)$" "\\1" PARTY_BOARD_WC_DESCRIBE "${PARTY_BOARD_WC_DESCRIBE}")
    string(REGEX REPLACE "-0$" "" PARTY_BOARD_WC_DESCRIBE "${PARTY_BOARD_WC_DESCRIBE}")

    # defines PARTY_BOARD_WC_BRANCH
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
            OUTPUT_VARIABLE PARTY_BOARD_WC_BRANCH
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    # defines PARTY_BOARD_WC_DATE
    execute_process(WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ${GIT_EXECUTABLE} log -1 --format=%ad
            OUTPUT_VARIABLE PARTY_BOARD_WC_DATE
            OUTPUT_STRIP_TRAILING_WHITESPACE)
else ()
    message(STATUS "Unable to find git, commit information will not be available")
endif ()

# A copy without .git (the Windows PC gets the code from the Mac): use the version recorded when it
# was copied. `git archive` fills in cmake/archive-version.txt (export-subst in .gitattributes); a
# plain file copy brings source-version.txt from dev/windows-build/source-version.sh instead.
set(PARTY_BOARD_VERSION_SOURCE "git")
if (NOT PARTY_BOARD_WC_REVISION)
    set(PARTY_BOARD_VERSION_SOURCE "none")
    foreach (partyboard_version_file cmake/archive-version.txt source-version.txt)
        set(partyboard_version_path "${CMAKE_SOURCE_DIR}/${partyboard_version_file}")
        if (NOT EXISTS "${partyboard_version_path}")
            continue()
        endif ()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${partyboard_version_path}")
        file(STRINGS "${partyboard_version_path}" partyboard_version_lines REGEX "^[a-z]+=")
        set(partyboard_version_revision "")
        set(partyboard_version_describe "")
        set(partyboard_version_branch "")
        set(partyboard_version_refs "")
        set(partyboard_version_date "")
        foreach (partyboard_version_line IN LISTS partyboard_version_lines)
            # Placeholders that were never filled in ($Format:...$) leave the value empty.
            if (partyboard_version_line MATCHES "^([a-z]+)=([^$]*)$")
                set(partyboard_version_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
            endif ()
        endforeach ()
        if (NOT partyboard_version_revision MATCHES "^[0-9a-f]+$")
            continue()
        endif ()
        set(PARTY_BOARD_WC_REVISION "${partyboard_version_revision}")
        set(PARTY_BOARD_WC_DATE "${partyboard_version_date}")
        # Same form as the git path above: drop the abbreviated hash (git describe --long, or plain
        # %(describe) away from a tag), keep -dirty, collapse a clean "-0".
        string(REGEX REPLACE "-g[0-9a-f]+(-dirty|)$" "\\1" PARTY_BOARD_WC_DESCRIBE "${partyboard_version_describe}")
        string(REGEX REPLACE "-0$" "" PARTY_BOARD_WC_DESCRIBE "${PARTY_BOARD_WC_DESCRIBE}")
        # git archive gives ref names ("HEAD -> feature/x, origin/feature/x, tag: ..."): the
        # checked-out branch, else the first branch.
        set(PARTY_BOARD_WC_BRANCH "${partyboard_version_branch}")
        if (partyboard_version_refs MATCHES "HEAD -> ([^,]+)")
            set(PARTY_BOARD_WC_BRANCH "${CMAKE_MATCH_1}")
        elseif (partyboard_version_refs)
            string(REPLACE ", " ";" partyboard_version_refs "${partyboard_version_refs}")
            foreach (partyboard_version_ref IN LISTS partyboard_version_refs)
                if (NOT partyboard_version_ref MATCHES "^(tag: |HEAD$)")
                    set(PARTY_BOARD_WC_BRANCH "${partyboard_version_ref}")
                    break()
                endif ()
            endforeach ()
        endif ()
        set(PARTY_BOARD_VERSION_SOURCE "${partyboard_version_file}")
        break()
    endforeach ()
    message(STATUS "No git checkout; version from ${PARTY_BOARD_VERSION_SOURCE}")
endif ()

if (PARTY_BOARD_WC_DESCRIBE MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9]+).*)?$")
    set(PARTY_BOARD_SHORT_VERSION_STRING "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    if (CMAKE_MATCH_5)
        set(PARTY_BOARD_VERSION_STRING "${PARTY_BOARD_SHORT_VERSION_STRING}.${CMAKE_MATCH_5}")
    else ()
        set(PARTY_BOARD_VERSION_STRING "${PARTY_BOARD_SHORT_VERSION_STRING}.0")
    endif ()
else ()
    set(PARTY_BOARD_WC_DESCRIBE "UNKNOWN-VERSION")
    set(PARTY_BOARD_VERSION_STRING "0.0.0.0")
    set(PARTY_BOARD_SHORT_VERSION_STRING "0.0.0")
endif ()

# Add version information to CI environment variables
if(DEFINED ENV{GITHUB_ENV})
    file(APPEND "$ENV{GITHUB_ENV}" "PARTY_BOAD_VERSION=${PARTY_BOARD_WC_DESCRIBE}\n")
endif()

message(STATUS "Party Board version set to ${PARTY_BOARD_WC_DESCRIBE}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
project(partyboard LANGUAGES C CXX VERSION ${PARTY_BOARD_VERSION_STRING})

option(PARTYBOARD_SWITCH_BOOTSTRAP
    "Build only the native Nintendo Switch/libnx bring-up target"
    OFF)

# The first Switch milestone deliberately stops before desktop-oriented
# dependencies (Aurora/Dawn/SDL3, RmlUi, nod, etc.) are configured. This keeps
# the libnx toolchain usable while those dependencies are audited and replaced
# or adapted one at a time. Normal desktop/mobile builds never enter this path.
if (PARTYBOARD_SWITCH_BOOTSTRAP)
    if (NOT PARTYBOARD_SWITCH)
        message(FATAL_ERROR
            "PARTYBOARD_SWITCH_BOOTSTRAP requires cmake/toolchains/switch-libnx.cmake")
    endif()
    add_subdirectory(platforms/switch)
    return()
endif()
if (APPLE)
    enable_language(OBJC)
endif ()
if (APPLE AND NOT TVOS AND CMAKE_SYSTEM_NAME STREQUAL tvOS)
    # ios.toolchain.cmake hack for SDL
    set(TVOS ON)
    set(IOS OFF)
endif ()

if(APPLE AND NOT CMAKE_OSX_SYSROOT)
    # If the Xcode SDK is lagging behind system version, CMake needs this done first
    execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
            OUTPUT_VARIABLE CMAKE_OSX_SYSROOT
            OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

# Aurora doesn't have prebuilts for x86_64 macOS and arm64 Windows yet
if (APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    set(AURORA_DAWN_PROVIDER "vendor" CACHE STRING "Dawn provider" FORCE)
endif ()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "_cmake")

include(FetchContent)

FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

include(cmake/rcheevos.cmake)
include(cmake/mbedtls.cmake)

set(VERSION 1)
set(PARTY_BOARD_BUNDLE_NAME PartyBoard)
set(PARTY_BOARD_BUNDLE_IDENTIFIER com.mariopartyrd.partyboard)
set(PARTY_BOARD_COMPANY_NAME "Mario Party R&D")
set(PARTY_BOARD_FILE_DESCRIPTION "Party Board")
set(PARTY_BOARD_PRODUCT_NAME "PartyBoard")
set(PARTY_BOARD_COPYRIGHT "Copyright (C) Mario Party R&D contributors")
# Windows file properties "Product version": the version plus branch and short revision.
string(REGEX REPLACE "^v" "" PARTY_BOARD_PRODUCT_VERSION_TEXT "${PARTY_BOARD_WC_DESCRIBE}")
if (PARTY_BOARD_WC_REVISION)
    string(SUBSTRING "${PARTY_BOARD_WC_REVISION}" 0 7 partyboard_short_revision)
    string(APPEND PARTY_BOARD_PRODUCT_VERSION_TEXT " (${PARTY_BOARD_WC_BRANCH} ${partyboard_short_revision})")
endif ()

option(USE_ASAN "Enable address sanitization" OFF)
option(SKIP_HU_ALLOC "Skip the game's built in allocator" OFF)
option(RUNTIME_SANITIZE "Runtime (stack) sanitization" OFF)
option(PARTYBOARD_EXPERIMENTAL_MUSYX_AUDIO "Enable the experimental MusyX audio backend" OFF)
# The HD texture beta (PartyBoard V1.4_Beta1 and later): sharper filtering of
# distant 3D textures, and replacement textures loaded from the app bundle's
# Resources/Textures. Built in its own build directory so the regular build,
# which friends must match byte for byte to play online, is unaffected.
option(PARTYBOARD_HD_BETA "Build the HD texture beta" OFF)
if(PARTYBOARD_HD_BETA)
    add_compile_definitions(PARTYBOARD_HD_BETA=1)
endif()

# ASan stays off by default, but the switch has to be reachable: this line used
# to FORCE it off, which silently discarded -DUSE_ASAN=ON and made the option
# below decorative. A build that asks for it explicitly now gets it.
if (NOT USE_ASAN)
    set(USE_ASAN OFF CACHE BOOL "Disable ASan" FORCE)
endif ()

if (USE_ASAN AND MSVC)
    # /fsanitize=address turns on the MSVC STL container annotations, and that
    # choice has to match across every object in the link. Only the game, the
    # port and the overlays are instrumented, so abseil, aurora, fmt and rmlui
    # disagreed and the link failed with LNK2038 mismatches on annotate_string,
    # annotate_vector and annotate_optional.
    #
    # _DISABLE_STL_ANNOTATION turns all of them off on both sides at once,
    # which is the documented remedy and covers any container Microsoft
    # annotates later. It costs exactly one class of detection: overflows
    # INSIDE a std::string, std::vector or std::optional buffer. Nothing this
    # investigation needs is affected, namely heap-buffer-overflow,
    # stack-buffer-overflow, global-buffer-overflow, use-after-free,
    # double-free and invalid-free. The corruption being hunted is in the
    # game's own arrays and in HuMem, not in an STL container.
    #
    # Set before any add_subdirectory or FetchContent call, so the dependencies
    # built from source pick it up too.
    add_compile_definitions(_DISABLE_STL_ANNOTATION=1)

    # Instrument everything that is built from source, not just our own code.
    # abseil changes the layout of raw_hash_set under
    # ABSL_HAVE_ADDRESS_SANITIZER, so an instrumented translation unit that
    # inlines the header and an uninstrumented library that defines the
    # out-of-line parts disagree about the object. That is an ODR violation,
    # and it showed up as a null dereference inside find_non_soo the moment
    # partyboard::config::Register touched the map at start-up. The same hazard
    # exists for any header-heavy dependency, so the flag goes on globally
    # before the subdirectories rather than target by target.
    #
    # Prebuilt packages (nod) and anything shipped as a binary cannot be
    # instrumented; that residual inconsistency is documented rather than
    # hidden, and it does not involve shared C++ templates.
    add_compile_options("/fsanitize=address")
endif ()
set(AURORA_ENABLE_DVD ON)
set(AURORA_ENABLE_RMLUI ON CACHE BOOL "Enable RmlUi UI support" FORCE)
set(RUNTIME_SANITIZE OFF)
set(SKIP_HU_ALLOC OFF CACHE BOOL "Skip the built in allocator" FORCE)

# Set build type to RelWithDebInfo if not specified
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif ()

if (APPLE)
    # dyld does not understand $ORIGIN (an ELF convention), so a macOS build
    # using it cannot find libdol.dylib and aborts at launch. Search only the
    # bundle's own Frameworks folder: a search path outside the app (beside it,
    # or the folder holding it) would let a stray dylib dropped there be loaded
    # into the game. Build and install rpaths match so install never rewrites
    # the binary and invalidates its ad-hoc signature; the build-tree app is
    # therefore not runnable, only the installed one.
    set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
    set(CMAKE_BUILD_RPATH "@executable_path/../Frameworks")
elseif (CMAKE_SYSTEM_NAME STREQUAL Linux)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
    set(CMAKE_BUILD_RPATH "$ORIGIN")
elseif (MSVC)
    add_compile_options(/bigobj)
endif ()

add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
add_subdirectory(extern/musyx EXCLUDE_FROM_ALL)
# MusyX carries the bank lifetime diagnostic's hooks, so it needs the port
# header that declares them. Added from here rather than inside the submodule so
# the submodule keeps building on its own.
target_include_directories(musyx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# dol is a SHARED library, so every static library linked into it has to be
# position independent. The submodule's own CMakeLists does not ask for that,
# and on Linux the link fails the moment MusyX's objects are really pulled in:
#
#   libmusyxRD.a(hw_pc.c.o): relocation R_X86_64_PC32 against symbol
#   `salAudioDumpFrameCount' can not be used when making a shared object
#
# Set from here, like the include path above, so the submodule keeps building
# on its own. A no-op on Windows.
set_property(TARGET musyx PROPERTY POSITION_INDEPENDENT_CODE ON)

# The pinned Tracy dependency already provides stb_image, which is used by the
# portable THP movie player for the JPEG video component.
FetchContent_GetProperties(tracy)

include(extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake)

configure_file(${CMAKE_SOURCE_DIR}/version.h.in ${CMAKE_BINARY_DIR}/partyboard_version.h)
include(files.cmake)

source_group("Game" FILES ${GAME_FILES})
source_group("Port" FILES ${PORT_FILES})

add_library(dol SHARED ${GAME_FILES} ${PORT_FILES} extern/libco/libco.c)

# Nayuki's QR Code generator (MIT, vendored at v1.8.0) for the phone
# controller's QR code in Settings > Input.
target_sources(dol PRIVATE extern/qrcodegen/qrcodegen.cpp)
target_include_directories(dol PRIVATE extern/qrcodegen)

# The game was built against the console libc's inline sqrtf, which returns a
# zero or negative argument unchanged; the system sqrtf returns NaN instead. That
# difference is what broke Makin' Waves' pool rim. Force-include a shim into the
# game's own C sources only -- not PORT_FILES, which share this target but are the
# port's C++, and not Aurora. See include/port/msl_sqrtf.h.
set(MSL_SQRTF_SHIM ${CMAKE_SOURCE_DIR}/include/port/msl_sqrtf.h)
if (MSVC)
    set(MSL_SQRTF_FLAG "/FI${MSL_SQRTF_SHIM}")
else ()
    set(MSL_SQRTF_FLAG -include ${MSL_SQRTF_SHIM})
endif ()
set_property(SOURCE ${GAME_FILES} APPEND PROPERTY COMPILE_OPTIONS ${MSL_SQRTF_FLAG})
# Test aid: extra compiler flags for the game's own C only (not the port, Aurora or third-party code),
# e.g. a sanitizer build: -DPARTYBOARD_EXTRA_GAME_C_FLAGS="-fsanitize=undefined;-fgnu89-inline". Empty by default.
set(PARTYBOARD_EXTRA_GAME_C_FLAGS "" CACHE STRING "Extra compiler flags for game C sources and RELs (test builds)")
# Where they apply: all, dol (the game files in the dol only), file:<regex of dol game file paths>,
# or a regex of REL target names (e.g. ^m450Dll$).
set(PARTYBOARD_EXTRA_GAME_C_FLAGS_SCOPE "all" CACHE STRING "all, dol, or a regex of REL target names")
if (PARTYBOARD_EXTRA_GAME_C_FLAGS AND (PARTYBOARD_EXTRA_GAME_C_FLAGS_SCOPE STREQUAL "all" OR PARTYBOARD_EXTRA_GAME_C_FLAGS_SCOPE STREQUAL "dol"))
    set_property(SOURCE ${GAME_FILES} APPEND PROPERTY COMPILE_OPTIONS ${PARTYBOARD_EXTRA_GAME_C_FLAGS})
elseif (PARTYBOARD_EXTRA_GAME_C_FLAGS AND PARTYBOARD_EXTRA_GAME_C_FLAGS_SCOPE MATCHES "^file:")
    # file:<regex> - only the dol's game files whose path matches (for bisecting)
    string(SUBSTRING "${PARTYBOARD_EXTRA_GAME_C_FLAGS_SCOPE}" 5 -1 _pb_file_re)
    set(_pb_files ${GAME_FILES})
    list(FILTER _pb_files INCLUDE REGEX "${_pb_file_re}")
    if (_pb_files)
        set_property(SOURCE ${_pb_files} APPEND PROPERTY COMPILE_OPTIONS ${PARTYBOARD_EXTRA_GAME_C_FLAGS})
    endif ()
endif ()
set_target_properties(dol PROPERTIES C_VISIBILITY_PRESET default CXX_VISIBILITY_PRESET default)
target_compile_definitions(dol PRIVATE TARGET_PC NON_MATCHING OPTIMIZED_TEXTURE_LOADING BYTESWAPPING TARGET_DOL VERSION=${VERSION} MUSY_VERSION_MAJOR=1 MUSY_VERSION_MINOR=5 MUSY_VERSION_PATCH=4)
target_include_directories(dol PRIVATE include extern extern/aurora/include extern/aurora/include/dolphin ${CMAKE_BINARY_DIR})
if (tracy_SOURCE_DIR)
    target_include_directories(dol PRIVATE ${tracy_SOURCE_DIR}/profiler/src)
endif()
target_link_libraries(dol PUBLIC aurora::core aurora::card aurora::dvd aurora::gx aurora::mtx aurora::os aurora::si aurora::vi aurora::pad musyx absl::flat_hash_map nlohmann_json::nlohmann_json fmt::fmt rmlui_debugger)
target_link_libraries(dol PRIVATE rcheevos mbedcrypto)
if (ANDROID)
    target_link_libraries(dol PRIVATE log)  # OSReport goes to logcat (src/port/stubs.c)
endif ()
# HTTPS for RetroAchievements, through each platform's own networking stack
# (include/port/http.hpp). Only the curl backend needs anything from the system;
# without it the feature is simply unavailable rather than breaking the build.
if (WIN32)
    target_link_libraries(dol PRIVATE winhttp)
elseif (APPLE)
    target_sources(dol PRIVATE src/port/http_apple.m)
    set_source_files_properties(src/port/http_apple.m PROPERTIES COMPILE_FLAGS -fobjc-arc)
    target_link_libraries(dol PRIVATE "-framework Foundation")
    # Online games disable minimizing (see the file for why).
    target_sources(dol PRIVATE src/port/button_prompts.cpp)
    target_sources(dol PRIVATE src/port/button_prompts_compose.m)
    set_source_files_properties(src/port/button_prompts_compose.m PROPERTIES COMPILE_FLAGS -fobjc-arc)
    target_sources(dol PRIVATE src/port/macos_window.m)
    set_source_files_properties(src/port/macos_window.m PROPERTIES COMPILE_FLAGS -fobjc-arc)
    # Menu > Send Logs.
    target_sources(dol PRIVATE src/port/send_logs.m)
    set_source_files_properties(src/port/send_logs.m PROPERTIES COMPILE_FLAGS -fobjc-arc)
elseif (NOT ANDROID)
    find_package(CURL QUIET)
    if (CURL_FOUND)
        target_compile_definitions(dol PRIVATE PARTYBOARD_HAVE_CURL)
        target_link_libraries(dol PRIVATE CURL::libcurl)
    else ()
        message(WARNING "libcurl not found: RetroAchievements will be unavailable in this build")
    endif ()
endif ()
if (WIN32)
    # Menu > Send Logs: zips the newest session logs (deflate through the zlib
    # Aurora already builds and ships as zlib1.dll) and shows the zip in File
    # Explorer (shell32/ole32).
    target_sources(dol PRIVATE src/port/send_logs_win.cpp)
    target_link_libraries(dol PRIVATE ZLIB::ZLIB shell32 ole32)
    # Controller button prompts, as on the Mac (APPLE block above), with the
    # portable composer: it assembles pictures the Mac composer drew
    # (tools/button-prompts/precompose.sh) and writes PNGs with libpng.
    target_sources(dol PRIVATE src/port/button_prompts.cpp src/port/button_prompts_compose.cpp)
    target_link_libraries(dol PRIVATE PNG::PNG)
    target_link_libraries(dol PUBLIC ws2_32)
    # Settings > Input > Phone Controller: this PC's address for the link.
    target_sources(dol PRIVATE src/port/lan_address_win.cpp)
    target_link_libraries(dol PRIVATE iphlpapi)
    # dbghelp is loaded lazily by the crash reporter for MiniDumpWriteDump, but
    # StackWalk64 and the symbol helpers are linked directly.
    target_link_libraries(dol PUBLIC dbghelp)
endif ()
# Give every libco coroutine stack its own guarded reservation (all platforms:
# on the Mac an unguarded stack overflow overwrote the heap) instead of a
# bare malloc block. libco falls back to malloc only when these are
# undefined (extern/libco/settings.h), so overriding them here needs no
# change inside libco itself. See include/port/coroutine_stack.h.
# libco.c is C and would otherwise have no prototype for the replacements,
# so the declarations are force-included into that one translation unit.
if (MSVC)
    set(PARTY_BOARD_LIBCO_FORCE_INCLUDE "/FIport/coroutine_stack_alloc.h")
else ()
    set(PARTY_BOARD_LIBCO_FORCE_INCLUDE "-include" "port/coroutine_stack_alloc.h")
endif ()
set_source_files_properties(extern/libco/libco.c PROPERTIES
        COMPILE_DEFINITIONS "LIBCO_MALLOC=PartyBoard_CoroutineStackAlloc;LIBCO_FREE=PartyBoard_CoroutineStackFree"
        COMPILE_OPTIONS "${PARTY_BOARD_LIBCO_FORCE_INCLUDE}")
# TODO it doesn't work on Linux yet
if (WIN32)
    if(USE_ASAN)
        target_compile_definitions(dol PRIVATE USE_ASAN)
        # The instrumentation flag itself is applied globally above, so the
        # game, the port, the overlays and every dependency built from source
        # are consistent. Only the USE_ASAN define is target-scoped.
    endif()
    if(SKIP_HU_ALLOC)
        target_compile_definitions(dol PRIVATE SKIP_HU_ALLOC)
    endif()
endif()
if (MSVC)
    target_link_options(dol PRIVATE "/DEF:${CMAKE_SOURCE_DIR}/dol.def")
    # Without this, editing the export list does not relink dol and the
    # executable fails on a symbol that looks like it was just added.
    set_property(TARGET dol APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/dol.def")
    # target_compile_options(dol PRIVATE "/WX")
    target_compile_options(dol PRIVATE "/wd4244") # implicit narrowing conversion
    target_compile_options(dol PRIVATE "/wd4101") # unreferenced local variable
    target_compile_options(dol PRIVATE "/wd4102") # unreferenced label
    target_compile_options(dol PRIVATE "/wd4305") # truncation
    target_compile_options(dol PRIVATE "/wd4018") # signed/unsigned mismatch
    target_compile_options(dol PRIVATE "/wd4996") # unsafe sprintf, etc
    target_compile_options(dol PRIVATE "/wd4146") # unary minus operator applied to unsigned type, result still unsigned
    target_compile_options(dol PRIVATE "/wd4552") # result of expression not used
    target_compile_options(dol PRIVATE "/wd4553") # result of expression not used: == vs =
    target_compile_options(dol PRIVATE "/wd4113") # function pointer parameter list mismatch
    # target_compile_options(dol PRIVATE "/Zi")
    #    target_compile_options(dol PRIVATE "/Ob0")
    target_compile_options(dol PRIVATE "/Od")
    if(NOT USE_ASAN AND RUNTIME_SANITIZE)
        target_compile_options(dol PRIVATE "/GS")
        target_compile_options(dol PRIVATE "/RTC1")  # incompatible with asan
    endif()
else ()
    # target_compile_options(dol PRIVATE "-fvisibility=hidden")
    # target_link_options(dol PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/dol.map")
endif ()

if(ANDROID)
    add_library(partyboard SHARED src/port/entry.cpp)
    set_target_properties(partyboard PROPERTIES OUTPUT_NAME main)
else ()
    add_executable(partyboard src/port/entry.cpp)
endif ()
target_compile_definitions(partyboard PRIVATE TARGET_PC NON_MATCHING VERSION=${VERSION})
target_include_directories(partyboard PRIVATE include ${CMAKE_BINARY_DIR})
target_link_libraries(partyboard PRIVATE dol aurora::main)
if (WIN32)
    # Session log folder + game.log + crash files (what the Mac launcher does),
    # and the phone controller's Python helper around the game.
    target_sources(partyboard PRIVATE src/port/session_log_win.cpp src/port/phone_bridge_win.cpp
            src/port/lan_address_win.cpp src/port/fastfail_catcher_win.cpp)
    target_link_libraries(partyboard PRIVATE shell32 ole32 advapi32 bcrypt iphlpapi dbghelp)
endif ()
aurora_copy_runtime_dlls(partyboard dol)

if (USE_ASAN AND MSVC)
    # The launcher itself is instrumented too, so the sanitizer runtime is
    # initialised before any game module loads rather than by whichever DLL
    # happens to come first.
    # ASan cannot coexist with incremental linking, and CMake turns that on by
    # default for RelWithDebInfo. Without this the link fails outright.
    foreach (asan_target dol partyboard)
        target_link_options(${asan_target} PRIVATE "/INCREMENTAL:NO")
    endforeach ()
    # Ship the sanitizer runtime beside the executable: the game is launched
    # from its output directory, not from a developer command prompt, so the
    # DLL is not otherwise on the search path.
    get_filename_component(PARTY_BOARD_MSVC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(PARTY_BOARD_ASAN_DLL "${PARTY_BOARD_MSVC_BIN}/clang_rt.asan_dynamic-x86_64.dll")
    if (EXISTS "${PARTY_BOARD_ASAN_DLL}")
        add_custom_command(TARGET partyboard POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PARTY_BOARD_ASAN_DLL}" "$<TARGET_FILE_DIR:partyboard>"
                COMMENT "Copying the AddressSanitizer runtime")
    else ()
        message(WARNING "AddressSanitizer runtime not found at ${PARTY_BOARD_ASAN_DLL};"
                " the instrumented build will not start.")
    endif ()
endif ()

list(APPEND BINARY_TARGETS partyboard dol)

set(REL_TARGETS "")
foreach (rel_file ${REL_FILES})
    get_filename_component(rel_dir ${rel_file} DIRECTORY)
    get_filename_component(rel_target ${rel_dir} NAME)
    list(APPEND REL_TARGETS ${rel_target})
endforeach ()
list(REMOVE_DUPLICATES REL_TARGETS)

# Add RELs
foreach (dir ${REL_TARGETS})
    if (IS_DIRECTORY ${CMAKE_SOURCE_DIR}/src/REL/${dir})
        set(REL_SOURCE_FILES ${REL_FILES})
        list(FILTER REL_SOURCE_FILES INCLUDE REGEX "^src/REL/${dir}/.*\\.c$")

        if (REL_SOURCE_FILES)
            add_library(${dir} SHARED ${REL_SOURCE_FILES})
            list(APPEND BINARY_TARGETS ${dir})
            if (USE_ASAN)
                target_compile_definitions(${dir} PRIVATE USE_ASAN)
                if (MSVC)
                    # Same incompatibility as the main targets: ASan and
                    # incremental linking cannot both be on.
                    target_link_options(${dir} PRIVATE "/INCREMENTAL:NO")
                endif ()
            endif()
            if (dir MATCHES "^w..Dll")
                target_sources(${dir} PRIVATE src/REL/board_executor.c)
            endif()
            if (WIN32)
                # This DLL's own C runtime: fatal calls report instead of
                # fast-failing silently (include/port/crt_fatal_handlers.h).
                target_sources(${dir} PRIVATE src/port/rel_crt_win.c)
            endif()
            target_compile_definitions(${dir} PRIVATE TARGET_PC OPTIMIZED_TEXTURE_LOADING BYTESWAPPING NON_MATCHING VERSION=${VERSION})
            target_compile_options(${dir} PRIVATE ${MSL_SQRTF_FLAG})
            if (PARTYBOARD_EXTRA_GAME_C_FLAGS AND (PARTYBOARD_EXTRA_GAME_C_FLAGS_SCOPE STREQUAL "all"
                    OR (NOT PARTYBOARD_EXTRA_GAME_C_FLAGS_SCOPE MATCHES "^(dol|file:)" AND dir MATCHES "${PARTYBOARD_EXTRA_GAME_C_FLAGS_SCOPE}")))
                # -fgnu89-inline (needed by the dol's own inline functions under a sanitizer) would
                # duplicate the inline functions RELs share through headers, so RELs leave it out.
                set(_pb_rel_flags ${PARTYBOARD_EXTRA_GAME_C_FLAGS})
                list(FILTER _pb_rel_flags EXCLUDE REGEX "gnu89-inline")
                target_compile_options(${dir} PRIVATE ${_pb_rel_flags})
            endif ()
            target_include_directories(${dir} PRIVATE include extern extern/aurora/include extern/aurora/include/dolphin)
            if (MSVC)
                set_target_properties(${dir} PROPERTIES LINK_FLAGS "/EXPORT:ObjectSetup")
                # target_compile_options(${dir} PRIVATE "/WX")
                target_compile_options(${dir} PRIVATE "/wd4244") # implicit narrowing conversion
                target_compile_options(${dir} PRIVATE "/wd4101") # unreferenced local variable
                target_compile_options(${dir} PRIVATE "/wd4102") # unreferenced label
                target_compile_options(${dir} PRIVATE "/wd4305") # truncation
                target_compile_options(${dir} PRIVATE "/wd4018") # signed/unsigned mismatch
                target_compile_options(${dir} PRIVATE "/wd4996") # unsafe sprintf, etc
                target_compile_options(${dir} PRIVATE "/wd4146") # unary minus operator applied to unsigned type, result still unsigned
                target_compile_options(${dir} PRIVATE "/wd4552") # result of expression not used
                target_compile_options(${dir} PRIVATE "/wd4553") # result of expression not used: == vs =
                target_compile_options(${dir} PRIVATE "/wd4113") # function pointer parameter list mismatch
                target_compile_options(${dir} PRIVATE "/wd4068") # unknown pragma
                # target_compile_options(${dir} PRIVATE "/Zi")
                #                target_compile_options(${dir} PRIVATE "/Ob0")
                target_compile_options(${dir} PRIVATE "/Od")
                if(NOT USE_ASAN AND RUNTIME_SANITIZE)
                    target_compile_options(${dir} PRIVATE "/GS")
                    target_compile_options(${dir} PRIVATE "/RTC1")  # incompatible with asan
                endif()
            elseif (NOT WIN32)
                # target_compile_options(${dir} PRIVATE "-fvisibility=hidden")
                if (APPLE)
                    target_link_options(${dir} PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/rel_symbols.txt")
                else()
                    target_link_options(${dir} PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/rel.map")
                endif()
                set_target_properties(${dir} PROPERTIES PREFIX "")
            endif ()
            target_link_libraries(${dir} PRIVATE dol musyx aurora::mtx aurora::gx)
            add_dependencies(partyboard ${dir})
        endif ()
    endif ()
endforeach ()

# Experimental cross-architecture measurement. Keep this opt-in until full
# games prove that Apple Silicon and Intel agree; it changes game arithmetic
# and therefore cannot be mixed with an existing released online build.
option(PARTYBOARD_NO_FMA "Disable fused multiply-add in game code for cross-platform tests" OFF)
if (PARTYBOARD_NO_FMA)
    # Named in the session log and the state log ("Netplay: build ..."); no
    # code changes behaviour on it.
    target_compile_definitions(dol PRIVATE PARTYBOARD_NO_FMA=1)
    # MSVC: /fp:strict would also rule out contraction, but under it MSVC
    # refuses to fold floating-point constants in static initialisers (C2099
    # in board/view.c, board/ui.c, hsfman.c, minigame_seq.c). /fp:precise keeps
    # IEEE source order and rounding and, since Visual Studio 2022, never
    # contracts (that takes /fp:contract); x64's default /arch:SSE2 has no FMA
    # instruction either. clang-cl is told explicitly.
    if (MSVC AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
        set(PARTYBOARD_NO_FMA_MSVC_FLAGS /fp:precise /clang:-ffp-contract=off)
    else ()
        set(PARTYBOARD_NO_FMA_MSVC_FLAGS /fp:precise)
    endif ()
    foreach (game_target dol ${REL_TARGETS} aurora_mtx)
        if (TARGET ${game_target})
            if (MSVC)
                target_compile_options(${game_target} PRIVATE ${PARTYBOARD_NO_FMA_MSVC_FLAGS})
            else ()
                target_compile_options(${game_target} PRIVATE -ffp-contract=off)
            endif ()
        endif ()
    endforeach ()
endif ()


# Use one implementation for game trigonometry across architectures. Opt-in
# until cross-chip gameplay and timing stress tests have passed.
option(PARTYBOARD_SHARED_MATH "Use shared fdlibm game maths for cross-platform tests" OFF)
if (PARTYBOARD_SHARED_MATH)
    if (NOT PARTYBOARD_NO_FMA)
        message(FATAL_ERROR "PARTYBOARD_SHARED_MATH requires PARTYBOARD_NO_FMA=ON")
    endif ()
    file(GLOB PARTYBOARD_SHARED_MATH_SOURCES CONFIGURE_DEPENDS src/port/shared_math/*.c)
    add_library(partyboard_shared_math STATIC ${PARTYBOARD_SHARED_MATH_SOURCES})
    set_target_properties(partyboard_shared_math PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET default)
    if (MSVC)
        target_compile_options(partyboard_shared_math PRIVATE ${PARTYBOARD_NO_FMA_MSVC_FLAGS})
        set(PARTYBOARD_SHARED_MATH_INCLUDE "/FI${CMAKE_SOURCE_DIR}/include/port/shared_math.h")
        set(PARTYBOARD_SHARED_MATH_TARGET_INCLUDE "${PARTYBOARD_SHARED_MATH_INCLUDE}")
        foreach (math_symbol sin sinf cos cosf tan tanf atan atanf asin asinf acos acosf pow powf atan2 atan2f fmod fmodf)
            target_link_options(dol PRIVATE "/EXPORT:pbd_${math_symbol}")
        endforeach ()
    else ()
        target_compile_options(partyboard_shared_math PRIVATE -ffp-contract=off -fno-strict-aliasing -fno-builtin)
        set(PARTYBOARD_SHARED_MATH_INCLUDE -include ${CMAKE_SOURCE_DIR}/include/port/shared_math.h)
        # Group the pair so CMake does not deduplicate this -include against
        # the separate console sqrtf shim on REL targets.
        set(PARTYBOARD_SHARED_MATH_TARGET_INCLUDE "SHELL:-include ${CMAKE_SOURCE_DIR}/include/port/shared_math.h")
    endif ()
    target_link_libraries(dol PRIVATE partyboard_shared_math)
    target_compile_definitions(dol PRIVATE PARTYBOARD_SHARED_MATH=1)
    set_property(SOURCE ${GAME_FILES} APPEND PROPERTY COMPILE_OPTIONS ${PARTYBOARD_SHARED_MATH_INCLUDE})
    foreach (rel_target ${REL_TARGETS})
        if (TARGET ${rel_target})
            target_compile_options(${rel_target} PRIVATE ${PARTYBOARD_SHARED_MATH_TARGET_INCLUDE})
        endif ()
    endforeach ()
    target_compile_options(aurora_mtx PRIVATE ${PARTYBOARD_SHARED_MATH_TARGET_INCLUDE})
endif ()

# Cross-play identity (docs/CROSS_PLATFORM_ONLINE.md, "Lobby"): the Tailscale lobby accepts only
# byte-identical builds, which a Mac and a Windows copy can never be. A cross-play build (both maths
# options ON) also carries a simulation fingerprint: a hash of every game source file, the
# simulation-relevant options and the revision. The lobby combines it with the netplay protocol and
# state-hash versions and accepts a guest whose binaries differ only when both fingerprints match.
# Builds without the cross-play options have none, so they keep the byte-exact check only.
set(PARTYBOARD_SIM_FINGERPRINT_HEADER ${CMAKE_BINARY_DIR}/partyboard_sim_fingerprint.h)
if (PARTYBOARD_NO_FMA AND PARTYBOARD_SHARED_MATH)
    set(partyboard_sim_options "no_fma=1 shared_math=1")
    foreach (partyboard_sim_option PARTYBOARD_HD_BETA PARTYBOARD_EXPERIMENTAL_MUSYX_AUDIO SKIP_HU_ALLOC USE_ASAN
            RUNTIME_SANITIZE PB_BOUNDS_BOARD)
        if (${partyboard_sim_option})
            string(APPEND partyboard_sim_options " ${partyboard_sim_option}=1")
        else ()
            string(APPEND partyboard_sim_options " ${partyboard_sim_option}=0")
        endif ()
    endforeach ()
    file(GLOB_RECURSE partyboard_sim_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES false RELATIVE ${CMAKE_SOURCE_DIR}
        src/*.c src/*.h src/*.cpp src/*.hpp src/*.cc src/*.cp src/*.inc src/*.m src/*.mm src/*.s src/*.txt src/*.cmake
        include/*.h include/*.hpp include/*.inc
        extern/*.c extern/*.h extern/*.cpp extern/*.hpp extern/*.cc extern/*.cp extern/*.inc extern/*.m extern/*.mm
        extern/*.s extern/*.txt extern/*.cmake
        libc/*.h libs/*.c libs/*.h libs/*.txt cmake/*.cmake)
    list(APPEND partyboard_sim_inputs CMakeLists.txt files.cmake dol.def rel.map rel_symbols.txt version.h.in)
    list(SORT partyboard_sim_inputs)
    list(REMOVE_DUPLICATES partyboard_sim_inputs)
    list(JOIN partyboard_sim_inputs "\n" partyboard_sim_list_text)
    set(partyboard_sim_list ${CMAKE_BINARY_DIR}/partyboard_sim_inputs.txt)
    file(CONFIGURE OUTPUT ${partyboard_sim_list} CONTENT "${partyboard_sim_list_text}\n" @ONLY)
    list(TRANSFORM partyboard_sim_inputs PREPEND ${CMAKE_SOURCE_DIR}/ OUTPUT_VARIABLE partyboard_sim_input_paths)
    add_custom_command(OUTPUT ${PARTYBOARD_SIM_FINGERPRINT_HEADER}
        COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR} -DLIST=${partyboard_sim_list}
            "-DOPTIONS=${partyboard_sim_options}" "-DREVISION=${PARTY_BOARD_WC_REVISION}"
            -DOUT=${PARTYBOARD_SIM_FINGERPRINT_HEADER} -P ${CMAKE_SOURCE_DIR}/cmake/sim_fingerprint.cmake
        DEPENDS ${partyboard_sim_input_paths} ${partyboard_sim_list}
        COMMENT "Cross-play simulation fingerprint"
        VERBATIM)
    target_sources(dol PRIVATE ${PARTYBOARD_SIM_FINGERPRINT_HEADER})
    target_compile_definitions(dol PRIVATE PARTYBOARD_CROSSPLAY_IDENTITY=1)
    message(STATUS "Cross-play identity ON: ${partyboard_sim_options}")
endif ()

if (ANDROID)
    # SDLActivity loads SDL_main via dlsym on Android. Since aurora::main is a static
    # archive, force an undefined reference so the linker keeps the SDL_main object.
    target_link_options(partyboard PRIVATE "-Wl,-u,SDL_main")
endif ()

if (NOT APPLE)
    # Depends on the files themselves, not on the executable being relinked:
    # as a POST_BUILD step, a change to a stylesheet alone never reached the
    # build directory, and the game kept showing the old one.
    file(GLOB_RECURSE PARTY_BOARD_RES_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/res/*")
    set(PARTY_BOARD_RES_STAMP ${CMAKE_BINARY_DIR}/res.stamp)
    add_custom_command(OUTPUT ${PARTY_BOARD_RES_STAMP}
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/res"
            "$<TARGET_FILE_DIR:partyboard>/res"
            COMMAND ${CMAKE_COMMAND} -E touch ${PARTY_BOARD_RES_STAMP}
            DEPENDS ${PARTY_BOARD_RES_FILES}
            COMMENT "Copying resources"
            VERBATIM
    )
    add_custom_target(partyboard_resources ALL DEPENDS ${PARTY_BOARD_RES_STAMP})
    add_dependencies(partyboard partyboard_resources)
endif ()
#
if (WIN32)
    set(PARTY_BOARD_WINDOWS_RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platforms/windows)
    set(PARTY_BOARD_WINDOWS_ICON_PNG ${CMAKE_CURRENT_SOURCE_DIR}/res/icon.png)
    # Pre-built from res/icon.png (16-256 px, PNG entries) so the build runs no
    # PowerShell script; regenerate it if the icon changes.
    set(PARTY_BOARD_WINDOWS_ICON_ICO ${PARTY_BOARD_WINDOWS_RESOURCE_DIR}/partyboard.ico)
    set(PARTY_BOARD_WINDOWS_RC ${CMAKE_CURRENT_BINARY_DIR}/partyboard.rc)
    set(PARTY_BOARD_WINDOWS_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/partyboard.manifest)


    configure_file(${PARTY_BOARD_WINDOWS_RESOURCE_DIR}/partyboard.manifest.in ${PARTY_BOARD_WINDOWS_MANIFEST} @ONLY)
    configure_file(${PARTY_BOARD_WINDOWS_RESOURCE_DIR}/partyboard.rc.in ${PARTY_BOARD_WINDOWS_RC} @ONLY)

    target_sources(partyboard PRIVATE ${PARTY_BOARD_WINDOWS_ICON_ICO} ${PARTY_BOARD_WINDOWS_RC})
    #    set_target_properties(partyboard PROPERTIES WIN32_EXECUTABLE TRUE)

    if (MSVC)
        target_link_options(partyboard PRIVATE /MANIFEST:NO)
    endif ()
endif ()

if (APPLE)
    if (IOS)
        set(PARTY_BOARD_RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platforms/ios)
    elseif (TVOS)
        set(PARTY_BOARD_RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platforms/tvos)
    else ()
        set(PARTY_BOARD_RESOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platforms/macos)
    endif ()
    set(PARTY_BOARD_INFO_PLIST ${PARTY_BOARD_RESOURCE_DIR}/Info.plist.in)
    file(GLOB_RECURSE PARTY_BOARD_RESOURCE_FILES
            "${PARTY_BOARD_RESOURCE_DIR}/Assets.car"
            "${PARTY_BOARD_RESOURCE_DIR}/Base.lproj/*"
            "${PARTY_BOARD_RESOURCE_DIR}/PartyBoard.icns")
    file(GLOB_RECURSE PARTY_BOARD_APP_RESOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/res/*")
    target_sources(partyboard PRIVATE ${PARTY_BOARD_RESOURCE_FILES})
    target_sources(partyboard PRIVATE ${PARTY_BOARD_APP_RESOURCE_FILES})
    foreach (FILE ${PARTY_BOARD_RESOURCE_FILES})
        file(RELATIVE_PATH NEW_FILE "${PARTY_BOARD_RESOURCE_DIR}" ${FILE})
        get_filename_component(NEW_FILE_PATH ${NEW_FILE} DIRECTORY)
        set_property(SOURCE ${FILE} PROPERTY MACOSX_PACKAGE_LOCATION "Resources/${NEW_FILE_PATH}")
    endforeach ()
    foreach (FILE ${PARTY_BOARD_APP_RESOURCE_FILES})
        file(RELATIVE_PATH NEW_FILE "${CMAKE_CURRENT_SOURCE_DIR}" ${FILE})
        get_filename_component(NEW_FILE_PATH ${NEW_FILE} DIRECTORY)
        set_property(SOURCE ${FILE} PROPERTY MACOSX_PACKAGE_LOCATION "Resources/${NEW_FILE_PATH}")
    endforeach ()
    set_target_properties(
            partyboard PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME ${PARTY_BOARD_BUNDLE_NAME}
            MACOSX_BUNDLE_GUI_IDENTIFIER ${PARTY_BOARD_BUNDLE_IDENTIFIER}
            MACOSX_BUNDLE_BUNDLE_VERSION ${PARTY_BOARD_VERSION_STRING}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${PARTY_BOARD_SHORT_VERSION_STRING}
            MACOSX_BUNDLE_INFO_PLIST ${PARTY_BOARD_INFO_PLIST}
            OUTPUT_NAME PartyBoard
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "YES"
            XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "YES"
    )
endif ()

if (IOS)
    find_library(UIKIT_FRAMEWORK UIKit REQUIRED)
    find_library(UNIFORM_TYPE_IDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)
    target_sources(partyboard PRIVATE src/port/ios/FileSelectDialog.m)
    set_source_files_properties(src/port/ios/FileSelectDialog.m PROPERTIES COMPILE_FLAGS -fobjc-arc)
    target_link_libraries(partyboard PRIVATE ${UIKIT_FRAMEWORK} ${UNIFORM_TYPE_IDENTIFIERS_FRAMEWORK})
endif ()


# Packaging logic
function(get_target_output_name target result_var)
    get_target_property(output_name ${target} OUTPUT_NAME)
    if (NOT output_name)
        set(${result_var} "${target}" PARENT_SCOPE)
    else ()
        set(${result_var} "${output_name}" PARENT_SCOPE)
    endif ()
endfunction()

function(get_target_prefix target result_var)
    set(${result_var} "" PARENT_SCOPE)
    if (APPLE)
        # Have to recreate some bundle logic here, since CMake can't tell us
        get_target_property(is_bundle ${target} MACOSX_BUNDLE)
        if (is_bundle)
            get_target_output_name(${target} output_name)
            if (CMAKE_SYSTEM_NAME STREQUAL Darwin)
                set(${result_var} "${output_name}.app/Contents/MacOS/" PARENT_SCOPE)
            else ()
                set(${result_var} "${output_name}.app/" PARENT_SCOPE)
            endif ()
        endif ()
    endif ()
endfunction()

set(EXTRA_TARGETS "")
if (TARGET crashpad_handler)
    list(APPEND EXTRA_TARGETS crashpad_handler)
endif ()

install(TARGETS ${BINARY_TARGETS} ${EXTRA_TARGETS} DESTINATION ${CMAKE_INSTALL_PREFIX})
# The line above puts libdol.dylib and every REL dylib beside PartyBoard.app,
# not in it, and the release zips only the .app -- so the shipped app had none
# of them. Put copies inside the bundle, where the rpath above finds them: dyld
# resolves libdol.dylib through it, and objdll.c's dlopen() of a bare REL name
# searches the same LC_RPATH entries.
if (APPLE AND CMAKE_SYSTEM_NAME STREQUAL Darwin)
    get_target_output_name(partyboard partyboard_output_name)
    set(BUNDLED_LIBRARY_TARGETS ${BINARY_TARGETS})
    list(REMOVE_ITEM BUNDLED_LIBRARY_TARGETS partyboard)
    install(TARGETS ${BUNDLED_LIBRARY_TARGETS}
            LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/${partyboard_output_name}.app/Contents/Frameworks)
endif ()
aurora_install_runtime_dlls(partyboard ${CMAKE_INSTALL_PREFIX})
# RmlUi is fetched EXCLUDE_FROM_ALL, so it is built but never installed. Where
# the build uses shared libraries (the Linux presets), the executable then
# needs a librmlui.so that is not in the install tree, and linuxdeploy cannot
# package the AppImage ("Could not find dependency: librmlui.so").
foreach (rmlui_target rmlui_core rmlui_debugger)
    if (TARGET ${rmlui_target})
        get_target_property(rmlui_type ${rmlui_target} TYPE)
        if (rmlui_type STREQUAL "SHARED_LIBRARY")
            install(TARGETS ${rmlui_target} LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX} RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX})
        endif ()
    endif ()
endforeach ()

if (NOT APPLE)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/res DESTINATION ${CMAKE_INSTALL_PREFIX})
endif ()
# Controller button prompt art beside partyboard.exe, where button_prompts.cpp
# looks (SDL_GetBasePath): the same files the Mac app has in
# Contents/Resources/ButtonPrompts, plus each family's composed/ folder of
# pictures the Mac composer drew for button_prompts_compose.cpp.
if (WIN32)
    set(PARTYBOARD_BUTTON_PROMPTS ${CMAKE_SOURCE_DIR}/../tools/button-prompts)
    install(DIRECTORY ${PARTYBOARD_BUTTON_PROMPTS}/art/ DESTINATION ${CMAKE_INSTALL_PREFIX}/ButtonPrompts)
    foreach (family xbox playstation switch)
        install(DIRECTORY ${PARTYBOARD_BUTTON_PROMPTS}/composed/${family}/
                DESTINATION ${CMAKE_INSTALL_PREFIX}/ButtonPrompts/${family}/composed)
    endforeach ()
endif ()

if (WIN32)
    # Debug symbols beside each module, so a crash report on a player's PC
    # names functions and source lines (src/port/coroutine_stack.cpp points
    # DbgHelp at the exe folder). The Debug-only block further down already
    # does this for Debug builds.
    if (MSVC AND NOT CMAKE_BUILD_TYPE STREQUAL Debug)
        foreach (target IN LISTS BINARY_TARGETS)
            install(FILES $<TARGET_PDB_FILE:${target}> DESTINATION ${CMAKE_INSTALL_PREFIX} OPTIONAL)
        endforeach ()
    endif ()
    # Which kind of build this install is, for dev/windows-build/package.cmake
    # (an HD beta's zip is named "... Beta <n> (Upscaled) Windows").
    if (PARTYBOARD_HD_BETA)
        set(PARTYBOARD_BUILD_FLAVOR "hd-beta")
    else ()
        set(PARTYBOARD_BUILD_FLAVOR "regular")
    endif ()
    install(CODE "file(WRITE \"${CMAKE_INSTALL_PREFIX}/build-flavor.txt\" \"${PARTYBOARD_BUILD_FLAVOR}\\n\")")
endif ()

# HD beta on Windows: the replacement texture set goes in a Textures folder
# beside partyboard.exe, where src/port/portmain.cpp (load_hd_textures) looks.
# The set (faithful/, font/, upscaled/; about 120 MB of PNGs made from the
# game's own art) is not in this repository; it comes from the Mac HD beta app's
# Contents/Resources/Textures and is copied to the PC (docs/WINDOWS_PC_SETUP.md).
# dev/windows-build/build-offline.cmd passes <PB root>\Textures when it exists.
if (WIN32 AND PARTYBOARD_HD_BETA)
    set(PARTYBOARD_HD_TEXTURES_DIR "" CACHE PATH
        "HD texture set (folder holding faithful/, font/, upscaled/) installed as Textures beside partyboard.exe")
    file(TO_CMAKE_PATH "${PARTYBOARD_HD_TEXTURES_DIR}" PARTYBOARD_HD_TEXTURES_SOURCE)
    if (PARTYBOARD_HD_TEXTURES_SOURCE AND IS_DIRECTORY "${PARTYBOARD_HD_TEXTURES_SOURCE}")
        file(GLOB_RECURSE PARTYBOARD_HD_TEXTURE_FILES LIST_DIRECTORIES false
            "${PARTYBOARD_HD_TEXTURES_SOURCE}/*.png" "${PARTYBOARD_HD_TEXTURES_SOURCE}/*.dds")
        list(LENGTH PARTYBOARD_HD_TEXTURE_FILES PARTYBOARD_HD_TEXTURE_COUNT)
        message(STATUS "HD textures: ${PARTYBOARD_HD_TEXTURE_COUNT} files from ${PARTYBOARD_HD_TEXTURES_SOURCE} will be installed as Textures")
        # Replace rather than merge, so a texture removed from the set (a
        # reverted upscale) does not linger in the install folder.
        install(CODE "file(REMOVE_RECURSE \"${CMAKE_INSTALL_PREFIX}/Textures\")")
        install(DIRECTORY "${PARTYBOARD_HD_TEXTURES_SOURCE}/" DESTINATION ${CMAKE_INSTALL_PREFIX}/Textures
            FILES_MATCHING PATTERN "*.png" PATTERN "*.dds")
    else ()
        message(WARNING "HD beta without PARTYBOARD_HD_TEXTURES_DIR (\"${PARTYBOARD_HD_TEXTURES_DIR}\"): the install "
            "gets no Textures folder, so the game shows its original textures. See docs/WINDOWS_PC_SETUP.md.")
    endif ()
endif ()

# The phone controller's helper, next to partyboard.exe in PhoneController\:
# the session supervisor runs it with Python (src/port/phone_bridge_win.cpp).
# (On the Mac it goes into the app's Contents/Resources by hand, with the
# launcher script; see "PartyBoard Phone Controller/".)
if (WIN32)
    set(PARTYBOARD_PHONE_CONTROLLER_DIR "${CMAKE_SOURCE_DIR}/../PartyBoard Phone Controller")
    if (EXISTS "${PARTYBOARD_PHONE_CONTROLLER_DIR}/bridge.py")
        install(FILES "${PARTYBOARD_PHONE_CONTROLLER_DIR}/bridge.py" "${PARTYBOARD_PHONE_CONTROLLER_DIR}/controller.html"
                DESTINATION ${CMAKE_INSTALL_PREFIX}/PhoneController)
    else ()
        message(WARNING "PartyBoard Phone Controller/ not found next to PartyBoard-build: "
                "the Windows install will have no phone controller")
    endif ()
endif ()

# Aurora's seed_pipeline_cache() reads this from beside the executable and merges
# it into the live cache. (src/port/portmain.cpp has an EnsureInitialPipelineCache()
# that copies it too, but into SDL_GetPrefPath("MarioPartyRD", ...) while Aurora
# keeps its cache in SDL_GetPrefPath(nullptr, ...) -- one level shallower -- so that
# copy lands where nothing reads it. Aurora's own path is the one that works.)
# Without a seed every
# player starts from an empty cache, and a pipeline that is still compiling does not
# delay its draw -- it drops it (extern/aurora/lib/gx/pipeline.cpp), so a scene seen
# for the first time is missing geometry for a few seconds.
#
# OPTIONAL because the file is a recording, not a build product: produce one with
# tools/capture_pipeline_cache.ps1 after playing through the scenes worth seeding.
# A tree without one still builds, and the port logs that none was found.
install(FILES ${CMAKE_SOURCE_DIR}/initial_pipeline_cache.db
        DESTINATION ${CMAKE_INSTALL_PREFIX} OPTIONAL)
# A macOS app looks for it in its own Contents/Resources, not beside the .app.
if (APPLE AND CMAKE_SYSTEM_NAME STREQUAL Darwin)
    install(FILES ${CMAKE_SOURCE_DIR}/initial_pipeline_cache.db
            DESTINATION ${CMAKE_INSTALL_PREFIX}/${partyboard_output_name}.app/Contents/Resources OPTIONAL)
endif ()

# The install rule alone only helps a packaged build. A developer running from
# the build directory gets whatever copy happens to sit there, which is how a
# stale 207-pipeline seed was still being loaded after the recording had been
# widened to 437 -- silently, since the engine reports how many rows merged but
# not which generation of the file they came from. Keep the build directory in
# step instead of relying on remembering to copy it by hand.
if (EXISTS ${CMAKE_SOURCE_DIR}/initial_pipeline_cache.db)
    # Depends on the seed itself rather than hanging off partyboard's POST_BUILD:
    # a POST_BUILD only fires when the executable relinks, so recapturing the seed
    # without touching any source would leave the stale copy in place -- which is
    # precisely the failure this is here to prevent.
    set(PIPELINE_SEED_STAGED ${CMAKE_BINARY_DIR}/initial_pipeline_cache.db)
    add_custom_command(OUTPUT ${PIPELINE_SEED_STAGED}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/initial_pipeline_cache.db
                ${PIPELINE_SEED_STAGED}
        DEPENDS ${CMAKE_SOURCE_DIR}/initial_pipeline_cache.db
        COMMENT "Staging the pipeline cache seed next to partyboard")
    add_custom_target(stage_pipeline_seed ALL DEPENDS ${PIPELINE_SEED_STAGED})
    add_dependencies(partyboard stage_pipeline_seed)
endif()

# TODO uncomment
if (CMAKE_BUILD_TYPE STREQUAL Debug) #OR CMAKE_BUILD_TYPE STREQUAL RelWithDebInfo)
    set(DEBUG_FILES_LIST "")
    foreach (target IN LISTS BINARY_TARGETS EXTRA_TARGETS)
        get_target_output_name(${target} output_name)
        if (WIN32)
            install(FILES $<TARGET_PDB_FILE:${target}> DESTINATION ${CMAKE_INSTALL_PREFIX} OPTIONAL)
        elseif (APPLE)
            get_target_prefix(${target} target_prefix)
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND rm -fr \"$<TARGET_FILE_NAME:${target}>.dSYM\")")
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND dsymutil \"${target_prefix}$<TARGET_FILE_NAME:${target}>\")")
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND strip -S \"${target_prefix}$<TARGET_FILE_NAME:${target}>\")")
            if (NOT target_prefix STREQUAL "")
                install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND mv \"${target_prefix}$<TARGET_FILE_NAME:${target}>.dSYM\" .)")
            endif ()
        elseif (UNIX)
            get_target_prefix(${target} target_prefix)
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND objcopy --only-keep-debug \"${target_prefix}$<TARGET_FILE_NAME:${target}>\" \"${target_prefix}$<TARGET_FILE_NAME:${target}>.dbg\")")
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND objcopy --strip-debug --add-gnu-debuglink=$<TARGET_FILE_NAME:${target}>.dbg \"${target_prefix}$<TARGET_FILE_NAME:${target}>\")")
        endif ()
        list(APPEND DEBUG_FILES_LIST "${output_name}")
    endforeach ()
    # This is a terrible hack to only run this on CI
    # until I turn this into a script or something
    if(DEFINED ENV{GITHUB_ENV})
        if (WIN32)
            list(TRANSFORM DEBUG_FILES_LIST APPEND ".pdb")
            list(JOIN DEBUG_FILES_LIST " " DEBUG_FILES)
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND 7z a -t7z \"${CMAKE_INSTALL_PREFIX}/debug.7z\" ${DEBUG_FILES})")
        elseif (APPLE)
            list(TRANSFORM DEBUG_FILES_LIST APPEND ".dSYM")
            list(JOIN DEBUG_FILES_LIST " " DEBUG_FILES)
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND tar acfv \"${CMAKE_INSTALL_PREFIX}/debug.tar.xz\" ${DEBUG_FILES})")
        elseif (UNIX)
            list(TRANSFORM DEBUG_FILES_LIST APPEND ".dbg")
            list(JOIN DEBUG_FILES_LIST " " DEBUG_FILES)
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND tar -I \"xz -9 -T0\" -cvf \"${CMAKE_INSTALL_PREFIX}/debug.tar.xz\" ${DEBUG_FILES})")
        endif ()
    endif ()
endif ()
foreach (target IN LISTS BINARY_TARGETS)
    get_target_prefix(${target} target_prefix)
    foreach (extra_target IN LISTS EXTRA_TARGETS)
        get_target_prefix(${extra_target} extra_prefix)
        if (NOT "${target_prefix}" STREQUAL "${extra_prefix}")
            # Copy extra target to target prefix
            install(CODE "execute_process(WORKING_DIRECTORY \"${CMAKE_INSTALL_PREFIX}\" COMMAND cp \"${extra_prefix}$<TARGET_FILE_NAME:${extra_target}>\" \"${target_prefix}$<TARGET_FILE_NAME:${extra_target}>\")")
        endif ()
    endforeach ()
endforeach ()

# Native online, CubeShelf, screen controller and disc version unit tests. Not part of the default build; CI builds and runs them
# (cmake --build <dir> --target online_tests, then run each from <dir>/tests).
add_executable(online_invitation_test EXCLUDE_FROM_ALL tools/tests/online_invitation_test.cpp src/port/online/invitation.cpp)
target_include_directories(online_invitation_test PRIVATE include)
add_executable(cubeshelf_social_test EXCLUDE_FROM_ALL tools/tests/cubeshelf_social_test.cpp src/port/online/cubeshelf_social.cpp)
target_include_directories(cubeshelf_social_test PRIVATE include)
target_link_libraries(cubeshelf_social_test PRIVATE mbedcrypto nlohmann_json::nlohmann_json)
add_executable(touch_controls_test EXCLUDE_FROM_ALL tools/tests/touch_controls_test.cpp src/port/touch_controls.cpp)
target_include_directories(touch_controls_test PRIVATE include extern/aurora/include)
target_compile_definitions(touch_controls_test PRIVATE TARGET_PC)
add_executable(disc_version_test EXCLUDE_FROM_ALL tools/tests/disc_version_test.cpp)
target_include_directories(disc_version_test PRIVATE include)
add_executable(app_update_test EXCLUDE_FROM_ALL tools/tests/app_update_test.cpp src/port/app_update_manifest.cpp)
target_include_directories(app_update_test PRIVATE include)
target_link_libraries(app_update_test PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(online_invitation_test cubeshelf_social_test touch_controls_test disc_version_test app_update_test PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_custom_target(online_tests DEPENDS online_invitation_test cubeshelf_social_test touch_controls_test disc_version_test app_update_test)

# Local debugging aid (8-player work): trap out-of-bounds array accesses in the
# board and board-overlay code only. Off unless -DPB_BOUNDS_BOARD=ON.
option(PB_BOUNDS_BOARD "Trap array overruns in board code (debug)" OFF)
if(PB_BOUNDS_BOARD)
    file(GLOB PB_BOUNDS_FILES ${CMAKE_SOURCE_DIR}/src/game/board/*.c ${CMAKE_SOURCE_DIR}/src/REL/w0*Dll/*.c ${CMAKE_SOURCE_DIR}/src/game/gamework.c)
    set_source_files_properties(${PB_BOUNDS_FILES} PROPERTIES COMPILE_OPTIONS "-fsanitize=bounds;-fsanitize-trap=bounds;-O0;-g")
endif()
