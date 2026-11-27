cmake_minimum_required(VERSION 3.20)
set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp" CACHE PATH
    "Path to psxrecomp")
if(NOT EXISTS "${PSXRECOMP_ROOT}/runtime/runtime.cmake")
    message(FATAL_ERROR
        "PSXRecomp runtime is missing: ${PSXRECOMP_ROOT}/runtime/runtime.cmake\n"
        "This project expects the psxrecomp framework checkout at ${PSXRECOMP_ROOT}.\n"
        "Run: git submodule update --init --recursive\n"
        "If psxrecomp/ is not a submodule, set -DPSXRECOMP_ROOT=<path-to-psxrecomp>.")
endif()

project(alundra_2 LANGUAGES C CXX)

# Local changes to the framework, kept as patches (the submodule is upstream's).
# Applied once at configure time; an already-applied patch is left alone.
file(GLOB _A2_FRAMEWORK_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/psxrecomp/*.patch")
list(SORT _A2_FRAMEWORK_PATCHES)
foreach(_patch IN LISTS _A2_FRAMEWORK_PATCHES)
    execute_process(COMMAND git apply --reverse --check "${_patch}"
                    WORKING_DIRECTORY "${PSXRECOMP_ROOT}"
                    RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)
    if(NOT _already EQUAL 0)
        execute_process(COMMAND git apply "${_patch}"
                        WORKING_DIRECTORY "${PSXRECOMP_ROOT}"
                        RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "Cannot apply framework patch ${_patch}")
        endif()
        message(STATUS "Applied framework patch ${_patch}")
    endif()
endforeach()

# --- Opt-in product surfaces (OFF by default in the framework) -------------
# Scaffold prompts / flags fill these. Without recomp-ui, PSX_RECOMP_UI is OFF.
# Setup-host CI requires WIZARD_CMAKE_BLOCK / ENABLE_SETUP_WIZARD (FORCE_SETUP_HOST
# alone does not open first-run / Generate & rebuild).
set(PSX_RECOMP_UI ON CACHE BOOL "Use recomp-ui" FORCE)
set(PSX_NETPLAY OFF CACHE BOOL "Netplay is not qualified" FORCE)
set(PSX_SETUP_WIZARD ON CACHE BOOL "Enable owned-input setup" FORCE)
set(PSXRECOMP_BIOS_STEMS "OpenBIOS;SCPH1001" CACHE STRING "OpenBIOS default, optional retail" FORCE)
set(PSXRECOMP_BIOS_STEM "OpenBIOS" CACHE STRING "Primary BIOS" FORCE)
set(PSXRECOMP_BIOS_PROFILE "${PSXRECOMP_ROOT}/bios/OpenBIOS.toml" CACHE FILEPATH "Primary BIOS" FORCE)
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

# This project's own mod catalog. The FRAMEWORK stages it -- together with its
# builtin psx.* packages -- into <exe-dir>/mods/bundled, and refuses to
# configure if a catalog exists here but is not declared. Never copy it by
# hand: five titles did, and framework 4cc04be3's rename of the staged
# directory broke all five silently (bead beads-eio.3.101).
set(PSX_PRELOADED_MODS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")
if(NOT IS_DIRECTORY "${PSX_PRELOADED_MODS}")
    set(PSX_PRELOADED_MODS "NONE")
endif()

psxrecomp_add_game_runtime(psx-runtime
    PRELOADED_MODS_DIR "${PSX_PRELOADED_MODS}"


    ENABLE_SETUP_WIZARD
    EXE_NAME "alundra_2"
    WINDOW_TITLE "Alundra 2"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
    MAX_PLAYERS 1
    APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico"


    GEN_MARKER "generated/SLES_026.01_dispatch.c"
    GEN_FULL_GLOB "generated/SLES_026.01_full_*.c"
    CODEGEN_SETUP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/codegen_setup.c"
)

# Stage runtime configs next to the exe for direct build/ launches. Only copy
# files that exist — never fail POST_BUILD when game_options.toml is absent.
foreach(_PSX_RUNTIME_CONFIG game.toml game_options.toml)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}")
        add_custom_command(TARGET psx-runtime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}"
                "$<TARGET_FILE_DIR:psx-runtime>/${_PSX_RUNTIME_CONFIG}"
            COMMENT "Staging ${_PSX_RUNTIME_CONFIG}"
            VERBATIM)
    endif()
endforeach()

# Hand-written replacements for guest functions (see src/ and docs/REWRITING.md).
file(GLOB A2_GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.c")
target_sources(psx-runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/input_override.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/a2_rewrite.c"
    ${A2_GAME_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/a2_lift_runtime.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/a2_diag.c")
# Game code translated from the MIPS by tools/a2lift.py (exact translation; hand-written
# rewrites in src/game/ take precedence). Generated locally from the player's disc, like
# generated/: without it, the original code runs where nothing was rewritten by hand.
file(GLOB A2_LIFTED_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/game/lifted/*.c")
if(A2_LIFTED_SOURCES)
    target_sources(psx-runtime PRIVATE ${A2_LIFTED_SOURCES})
    set_source_files_properties(${A2_LIFTED_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-value")
else()
    message(STATUS "src/game/lifted/ is empty: run tools/a2lift.py to translate the game code")
    target_sources(psx-runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/a2_lift_none.c")
endif()
target_include_directories(psx-runtime PRIVATE "${PSXRECOMP_ROOT}/runtime/include")
if(NOT WIN32 AND NOT APPLE)
    target_link_libraries(psx-runtime PRIVATE m)
endif()
