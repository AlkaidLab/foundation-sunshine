# Black Mode build profile.
#
# When SUNSHINE_BLACK_MODE=ON, the binary embeds:
#   - A built-in username (SUNSHINE_BLACK_USERNAME)
#   - A pre-computed SHA-256(password || salt) digest (NEVER the plaintext)
#   - An optional defaults conf snippet (SUNSHINE_BLACK_DEFAULTS_FILE)
#
# Plaintext password is supplied via ONE of these channels (in priority order):
#   1. -DSUNSHINE_BLACK_PASSWORD_HASH=<hex64> together with -DSUNSHINE_BLACK_SALT=<str>
#      (preferred for CI / scripted builds: plaintext never touches CMake at all).
#   2. Environment variable SUNSHINE_BLACK_PASSWORD before invoking cmake.
#      CMake reads it once, hashes it, and immediately unsets it from its own
#      process environment. The plaintext is never written to CMakeCache.txt.
#
# Helper to pre-compute the hash in PowerShell (sunshine util::hex format:
# uppercase, byte-reversed):
#   $salt   = -join ((48..57 + 65..90 + 97..122) | Get-Random -Count 16 | %{[char]$_})
#   $pw     = Read-Host "Password" -AsSecureString | %{
#               [System.Net.NetworkCredential]::new('', $_).Password }
#   $bytes  = [System.Text.Encoding]::UTF8.GetBytes($pw + $salt)
#   $digest = [System.Security.Cryptography.SHA256]::Create().ComputeHash($bytes)
#   [Array]::Reverse($digest)
#   $hash   = (-join ($digest | %{ '{0:X2}' -f $_ }))
#   "$salt  $hash"
#   cmake ... -DSUNSHINE_BLACK_SALT=$salt -DSUNSHINE_BLACK_PASSWORD_HASH=$hash

option(SUNSHINE_BLACK_MODE "Build with embedded Black Mode defaults and credentials." OFF)

# Sunshine's util::hex formats SHA-256 digests as UPPERCASE bytes printed in
# REVERSE byte order. We must mirror that here so the embedded digest matches
# what the runtime auth path computes.
function(_sunshine_util_hex_format IN_HEX OUT_VAR)
    string(LENGTH "${IN_HEX}" _len)
    set(_pairs "")
    set(_pos 0)
    while(_pos LESS _len)
        string(SUBSTRING "${IN_HEX}" ${_pos} 2 _pair)
        list(APPEND _pairs "${_pair}")
        math(EXPR _pos "${_pos} + 2")
    endwhile()
    list(REVERSE _pairs)
    list(JOIN _pairs "" _joined)
    string(TOUPPER "${_joined}" _upper)
    set(${OUT_VAR} "${_upper}" PARENT_SCOPE)
endfunction()

set(SUNSHINE_BLACK_USERNAME ""
        CACHE STRING "Built-in default username embedded into the binary when SUNSHINE_BLACK_MODE=ON.")
set(SUNSHINE_BLACK_SALT ""
        CACHE STRING "Salt for the Black Mode credential digest. Auto-generated if left empty.")
set(SUNSHINE_BLACK_PASSWORD_HASH ""
        CACHE STRING "Pre-computed SHA-256(password || salt) hex digest. If set, env var SUNSHINE_BLACK_PASSWORD is ignored.")
set(SUNSHINE_BLACK_DEFAULTS_FILE ""
        CACHE FILEPATH "Optional path to a sunshine.conf-format file whose contents are embedded as base defaults when SUNSHINE_BLACK_MODE=ON.")

if(SUNSHINE_BLACK_MODE)
    if(SUNSHINE_BLACK_USERNAME STREQUAL "")
        message(FATAL_ERROR "SUNSHINE_BLACK_MODE=ON requires -DSUNSHINE_BLACK_USERNAME=<name>.")
    endif()

    set(SUNSHINE_BLACK_MODE_ENABLED 1)

    # Auto-generate salt if user did not pin one.
    if(SUNSHINE_BLACK_SALT STREQUAL "")
        string(RANDOM LENGTH 16
                ALPHABET "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                _generated_salt)
        set(SUNSHINE_BLACK_SALT "${_generated_salt}"
                CACHE STRING "Salt for the Black Mode credential digest. Auto-generated if left empty." FORCE)
        unset(_generated_salt)
    endif()

    # Resolve the password hash without ever creating a cache entry for the plaintext.
    if(NOT SUNSHINE_BLACK_PASSWORD_HASH STREQUAL "")
        message(STATUS "Black Mode: using pre-computed password hash from -D.")
    elseif(DEFINED ENV{SUNSHINE_BLACK_PASSWORD})
        set(_black_plain "$ENV{SUNSHINE_BLACK_PASSWORD}")
        # Strip from this cmake process's environment so child processes (compile
        # invocations, sub-cmake, etc.) cannot see it.
        unset(ENV{SUNSHINE_BLACK_PASSWORD})

        if(_black_plain STREQUAL "")
            message(FATAL_ERROR "SUNSHINE_BLACK_PASSWORD environment variable is empty.")
        endif()

        string(SHA256 _black_raw "${_black_plain}${SUNSHINE_BLACK_SALT}")
        # Overwrite local variable before unsetting (defense in depth).
        set(_black_plain "")
        unset(_black_plain)

        _sunshine_util_hex_format("${_black_raw}" _black_hash)
        unset(_black_raw)

        set(SUNSHINE_BLACK_PASSWORD_HASH "${_black_hash}"
                CACHE STRING "SHA-256(password || salt) digest in sunshine util::hex format (uppercase, byte-reversed)." FORCE)
        unset(_black_hash)
        message(STATUS "Black Mode: hashed plaintext from SUNSHINE_BLACK_PASSWORD env var (plaintext was not cached).")
    else()
        message(FATAL_ERROR
                "SUNSHINE_BLACK_MODE=ON requires either -DSUNSHINE_BLACK_PASSWORD_HASH=<hex64> "
                "or the environment variable SUNSHINE_BLACK_PASSWORD before invoking cmake.")
    endif()

    string(LENGTH "${SUNSHINE_BLACK_PASSWORD_HASH}" _hash_len)
    if(NOT _hash_len EQUAL 64)
        message(FATAL_ERROR "SUNSHINE_BLACK_PASSWORD_HASH must be a 64-character SHA-256 digest in sunshine util::hex format (uppercase, byte-reversed). Got ${_hash_len} chars.")
    endif()
    # Normalize to uppercase in case the caller pre-computed it in lowercase.
    string(TOUPPER "${SUNSHINE_BLACK_PASSWORD_HASH}" _hash_upper)
    if(NOT _hash_upper STREQUAL "${SUNSHINE_BLACK_PASSWORD_HASH}")
        set(SUNSHINE_BLACK_PASSWORD_HASH "${_hash_upper}"
                CACHE STRING "SHA-256(password || salt) digest in sunshine util::hex format (uppercase, byte-reversed)." FORCE)
    endif()
    unset(_hash_upper)
    unset(_hash_len)

    message(STATUS "Black Mode: ENABLED (username=${SUNSHINE_BLACK_USERNAME})")

    if(NOT SUNSHINE_BLACK_DEFAULTS_FILE STREQUAL "")
        if(NOT EXISTS "${SUNSHINE_BLACK_DEFAULTS_FILE}")
            message(FATAL_ERROR "SUNSHINE_BLACK_DEFAULTS_FILE not found: ${SUNSHINE_BLACK_DEFAULTS_FILE}")
        endif()
        file(READ "${SUNSHINE_BLACK_DEFAULTS_FILE}" SUNSHINE_BLACK_DEFAULTS_CONTENT)
        message(STATUS "Black Mode: defaults conf = ${SUNSHINE_BLACK_DEFAULTS_FILE}")

        # Reconfigure when the defaults file changes.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SUNSHINE_BLACK_DEFAULTS_FILE}")
    else()
        set(SUNSHINE_BLACK_DEFAULTS_CONTENT "")
    endif()
else()
    set(SUNSHINE_BLACK_MODE_ENABLED 0)
    set(SUNSHINE_BLACK_USERNAME "")
    set(SUNSHINE_BLACK_SALT "")
    set(SUNSHINE_BLACK_PASSWORD_HASH "")
    set(SUNSHINE_BLACK_DEFAULTS_CONTENT "")
    message(STATUS "Black Mode: disabled (pass -DSUNSHINE_BLACK_MODE=ON to enable).")
endif()

configure_file(
        "${CMAKE_SOURCE_DIR}/src/black_config.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/black_config.h"
        @ONLY)
