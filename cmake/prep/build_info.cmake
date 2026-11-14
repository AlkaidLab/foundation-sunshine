# Build identity: what exactly this binary is, recorded at configure time and
# compiled into build_info_defs.h, the Windows VERSIONINFO (custom String
# keys) and `sunshine --version --json` / `GET /api/version`.
#
# Every field can be set by the build environment (CI); a local build fills
# them from git and the clock:
#   SUNSHINE_BUILD_COMMIT    full 40-hex commit           default: git rev-parse HEAD, else $COMMIT
#   SUNSHINE_BUILD_BRANCH    branch                       default: git, else $BRANCH
#   SUNSHINE_BUILD_DIRTY     0/1                          default: git diff --quiet HEAD
#   SUNSHINE_BUILD_CHANNEL   stable|beta|dev|local        default: local (CI passes beta or dev)
#   SUNSHINE_BUILD_VARIANT   product line                 default: foundation-sunshine
#   SUNSHINE_BUILD_PACKAGE   installer|portable|auto      default: auto (one build makes both packages;
#                                                        the running host decides from its install layout)
#   SUNSHINE_BUILD_BUILDER   ci|local                     default: ci when $CI or $GITHUB_ACTIONS is set
#   SUNSHINE_BUILD_CI_RUN    URL or id of the CI run      default: GitHub/Gitea run URL when available
# The build time is UTC ISO 8601 with Z; string(TIMESTAMP) honours
# SOURCE_DATE_EPOCH for reproducible builds.

function(_sunshine_build_info_value out env_name fallback)
    if(DEFINED ENV{${env_name}} AND NOT "$ENV{${env_name}}" STREQUAL "")
        set(${out} "$ENV{${env_name}}" PARENT_SCOPE)
    elseif(DEFINED ${env_name} AND NOT "${${env_name}}" STREQUAL "")
        set(${out} "${${env_name}}" PARENT_SCOPE)
    else()
        set(${out} "${fallback}" PARENT_SCOPE)
    endif()
endfunction()

find_package(Git QUIET)
set(_bi_git_commit "")
set(_bi_git_branch "")
set(_bi_git_dirty 0)
if(GIT_EXECUTABLE)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _bi_git_commit RESULT_VARIABLE _bi_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _bi_rc EQUAL 0)
        set(_bi_git_commit "")
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _bi_git_branch RESULT_VARIABLE _bi_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _bi_rc EQUAL 0)
        set(_bi_git_branch "")
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD --
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE _bi_rc ERROR_QUIET)
    if(_bi_rc EQUAL 1)
        set(_bi_git_dirty 1)
    endif()
endif()

# A source tree made with `git archive` has no .git: git writes the commit and its refs into
# build_identity_archive.txt instead (export-subst in .gitattributes). An archive of a commit is that commit
# exactly, so it is not dirty. In a checkout the file still holds the $Format placeholders and is ignored.
if(_bi_git_commit STREQUAL "" AND EXISTS "${CMAKE_SOURCE_DIR}/cmake/prep/build_identity_archive.txt")
    file(STRINGS "${CMAKE_SOURCE_DIR}/cmake/prep/build_identity_archive.txt" _bi_archive)
    list(LENGTH _bi_archive _bi_archive_lines)
    if(_bi_archive_lines GREATER 0)
        list(GET _bi_archive 0 _bi_archive_commit)
        if(_bi_archive_commit MATCHES "^[0-9a-f]+$")
            string(LENGTH "${_bi_archive_commit}" _bi_archive_len)
        else()
            set(_bi_archive_len 0)
        endif()
        if(_bi_archive_len EQUAL 40)
            set(_bi_git_commit "${_bi_archive_commit}")
            set(_bi_git_dirty 0)
            if(_bi_archive_lines GREATER 1)
                # %D: "HEAD -> feat/x, origin/feat/x, tag: v1" -> the first branch name
                list(GET _bi_archive 1 _bi_refs)
                string(REPLACE "HEAD -> " "" _bi_refs "${_bi_refs}")
                string(REPLACE ", " ";" _bi_refs "${_bi_refs}")
                foreach(_bi_ref IN LISTS _bi_refs)
                    if(NOT _bi_ref MATCHES "^(tag: |HEAD$)" AND _bi_git_branch STREQUAL "")
                        set(_bi_git_branch "${_bi_ref}")
                    endif()
                endforeach()
            endif()
        endif()
    endif()
endif()

# CI's COMMIT may be short or full; a full git answer wins over it.
set(_bi_commit_fallback "${_bi_git_commit}")
if(_bi_commit_fallback STREQUAL "" AND DEFINED ENV{COMMIT})
    set(_bi_commit_fallback "$ENV{COMMIT}")
endif()
set(_bi_branch_fallback "${_bi_git_branch}")
if(DEFINED ENV{BRANCH} AND NOT "$ENV{BRANCH}" STREQUAL "")
    set(_bi_branch_fallback "$ENV{BRANCH}")
endif()

set(_bi_builder_fallback "local")
if((DEFINED ENV{CI} AND NOT "$ENV{CI}" STREQUAL "") OR (DEFINED ENV{GITHUB_ACTIONS}))
    set(_bi_builder_fallback "ci")
endif()
set(_bi_ci_run_fallback "")
if(DEFINED ENV{GITHUB_RUN_ID} AND DEFINED ENV{GITHUB_SERVER_URL} AND DEFINED ENV{GITHUB_REPOSITORY})
    set(_bi_ci_run_fallback "$ENV{GITHUB_SERVER_URL}/$ENV{GITHUB_REPOSITORY}/actions/runs/$ENV{GITHUB_RUN_ID}")
endif()

_sunshine_build_info_value(BUILD_INFO_COMMIT SUNSHINE_BUILD_COMMIT "${_bi_commit_fallback}")
_sunshine_build_info_value(BUILD_INFO_BRANCH SUNSHINE_BUILD_BRANCH "${_bi_branch_fallback}")
_sunshine_build_info_value(BUILD_INFO_DIRTY SUNSHINE_BUILD_DIRTY "${_bi_git_dirty}")
_sunshine_build_info_value(BUILD_INFO_CHANNEL SUNSHINE_BUILD_CHANNEL "local")
_sunshine_build_info_value(BUILD_INFO_VARIANT SUNSHINE_BUILD_VARIANT "foundation-sunshine")
_sunshine_build_info_value(BUILD_INFO_PACKAGE SUNSHINE_BUILD_PACKAGE "auto")
_sunshine_build_info_value(BUILD_INFO_BUILDER SUNSHINE_BUILD_BUILDER "${_bi_builder_fallback}")
_sunshine_build_info_value(BUILD_INFO_CI_RUN SUNSHINE_BUILD_CI_RUN "${_bi_ci_run_fallback}")
string(TIMESTAMP BUILD_INFO_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

if(NOT BUILD_INFO_CHANNEL MATCHES "^(stable|beta|dev|local)$")
    message(FATAL_ERROR "SUNSHINE_BUILD_CHANNEL must be stable, beta, dev or local (got '${BUILD_INFO_CHANNEL}')")
endif()
if(NOT BUILD_INFO_PACKAGE MATCHES "^(installer|portable|auto)$")
    message(FATAL_ERROR "SUNSHINE_BUILD_PACKAGE must be installer, portable or auto (got '${BUILD_INFO_PACKAGE}')")
endif()
if(NOT BUILD_INFO_BUILDER MATCHES "^(ci|local)$")
    message(FATAL_ERROR "SUNSHINE_BUILD_BUILDER must be ci or local (got '${BUILD_INFO_BUILDER}')")
endif()
if(BUILD_INFO_DIRTY)
    set(BUILD_INFO_DIRTY 1)
else()
    set(BUILD_INFO_DIRTY 0)
endif()
if(NOT BUILD_INFO_COMMIT MATCHES "^[0-9a-f]*$")
    message(FATAL_ERROR "SUNSHINE_BUILD_COMMIT must be a lowercase hex commit id (got '${BUILD_INFO_COMMIT}')")
endif()
# Only quote-free text goes into the generated header and resource script.
foreach(_bi_field BRANCH VARIANT CI_RUN)
    string(REGEX REPLACE "[\"\\\\\r\n]" "_" BUILD_INFO_${_bi_field} "${BUILD_INFO_${_bi_field}}")
endforeach()

# VERSIONINFO: a local build is not built by the standard release procedure,
# so it carries VS_FF_PRIVATEBUILD and the PrivateBuild string the flag requires.
if(BUILD_INFO_BUILDER STREQUAL "local")
    set(BUILD_INFO_RC_FILEFLAGS "VS_FF_PRIVATEBUILD")
    set(BUILD_INFO_RC_PRIVATE_BUILD "            VALUE \"PrivateBuild\",     \"local build ${BUILD_INFO_BRANCH}\\0\"")
else()
    set(BUILD_INFO_RC_FILEFLAGS "0x0L")
    set(BUILD_INFO_RC_PRIVATE_BUILD "")
endif()

# The binary FILEVERSION / PRODUCTVERSION (4 x 16 bits) from the version string: YYYY.MMDD.HHMMSS -> YYYY,MMDD,HHMM,SS.
include(${CMAKE_MODULE_PATH}/prep/rc_version.cmake)
sunshine_rc_file_version(BUILD_INFO_RC_FILE_VERSION "${PROJECT_VERSION}")

message(STATUS "Build identity: ${BUILD_INFO_CHANNEL} ${BUILD_INFO_VARIANT} ${BUILD_INFO_COMMIT} "
        "dirty=${BUILD_INFO_DIRTY} ${BUILD_INFO_TIME_UTC} package=${BUILD_INFO_PACKAGE} builder=${BUILD_INFO_BUILDER}")
