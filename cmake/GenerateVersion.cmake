# Generates ${OUT_FILE} (version.h) from app/res/version.h.in's hand-maintained
# MAJOR/MINOR plus git state for REVISION.
#
# Run at BUILD time (not configure time) by the nifdiff_version custom target,
# so committing does not leave the exe stamped with a stale revision. The
# generated header is only rewritten when its content actually changes, so a
# no-op build stays a no-op instead of recompiling everything that includes it.
#
# Expects: SRC_DIR, OUT_FILE, TEMPLATE (passed with -D from CMakeLists).

set(_versionHeader "${SRC_DIR}/app/res/version.h.in")
if(NOT EXISTS "${_versionHeader}")
    message(FATAL_ERROR "NIFDiff: missing ${_versionHeader}")
endif()

file(STRINGS "${_versionHeader}" _nifdiffVersionDefines
    REGEX "^#define NIFDIFF_VERSION_(MAJOR|MINOR)[ \t]+[0-9]+")
foreach(component MAJOR MINOR)
    unset(NIFDIFF_VERSION_${component})
    foreach(line IN LISTS _nifdiffVersionDefines)
        if(line MATCHES "^#define NIFDIFF_VERSION_${component}[ \t]+([0-9]+)")
            set(NIFDIFF_VERSION_${component} "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(NOT DEFINED NIFDIFF_VERSION_${component})
        message(FATAL_ERROR
            "NIFDiff: NIFDIFF_VERSION_${component} is missing from ${_versionHeader}")
    endif()
endforeach()

find_package(Git QUIET)

set(NIFDIFF_VERSION_REVISION 0)
set(NIFDIFF_GIT_HASH "unknown")
set(NIFDIFF_GIT_DIRTY 0)

if(GIT_FOUND AND EXISTS "${SRC_DIR}/.git")
    # Revision = number of commits reachable from HEAD. Monotonic on a
    # linear history, which is what makes it usable as a version component.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE _count
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_count MATCHES "^[0-9]+$")
        set(NIFDIFF_VERSION_REVISION "${_count}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE _hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_hash)
        set(NIFDIFF_GIT_HASH "${_hash}")
    endif()

    # Uncommitted changes: the exe is then NOT reproducible from the hash, so
    # say so in the version string rather than letting a local build pass
    # itself off as the tagged one.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE _dirty
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT _dirty STREQUAL "")
        set(NIFDIFF_GIT_DIRTY 1)
    endif()
endif()

set(NIFDIFF_VERSION_STRING
    "${NIFDIFF_VERSION_MAJOR}.${NIFDIFF_VERSION_MINOR}.${NIFDIFF_VERSION_REVISION}")

if(NIFDIFF_GIT_DIRTY)
    set(NIFDIFF_VERSION_DISPLAY "${NIFDIFF_VERSION_STRING}+dev")
else()
    set(NIFDIFF_VERSION_DISPLAY "${NIFDIFF_VERSION_STRING}")
endif()

configure_file("${TEMPLATE}" "${OUT_FILE}.tmp" @ONLY)

# Rewrite only on change (configure_file always touches its output).
set(_write TRUE)
if(EXISTS "${OUT_FILE}")
    file(READ "${OUT_FILE}" _old)
    file(READ "${OUT_FILE}.tmp" _new)
    if(_old STREQUAL _new)
        set(_write FALSE)
    endif()
endif()
if(_write)
    file(RENAME "${OUT_FILE}.tmp" "${OUT_FILE}")
    message(STATUS "NIFDiff version -> ${NIFDIFF_VERSION_DISPLAY} (${NIFDIFF_GIT_HASH})")
else()
    file(REMOVE "${OUT_FILE}.tmp")
endif()
