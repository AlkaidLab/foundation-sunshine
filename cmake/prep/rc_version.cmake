# The 4 x 16-bit binary FILEVERSION / PRODUCTVERSION of the Windows VERSIONINFO, derived from the version string.
#
# The version string stays YYYY.MMDD.HHMMSS[.suffix] (FileVersion / ProductVersion strings, --version, the update
# metadata). Its third part, HHMMSS, does not fit a 16-bit field (235959 > 65535), so the binary form splits it:
#   YYYY.MMDD.HHMMSS  ->  YYYY, MMDD, HHMM, SS      e.g. 2026.1104.235959 -> 2026,1104,2359,59
# The order of the binary versions follows the order of the build times. Another numeric form a.b.c[.d] with every part
# <= 65535 is used as is; anything else (a manual CI tag) is 0,0,0,0, as all builds were before (the fields came from
# project(VERSION 0.0.0)). Nothing compares the binary fields today: the installer copies with ignoreversion, the panel,
# its updater and the doctor read the version string.
#
# sunshine_rc_file_version(<out> <version>) sets <out> to "a,b,c,d".
function(sunshine_rc_file_version out version)
    set(result "0,0,0,0")
    if(version MATCHES "^[vV]?([0-9][0-9][0-9][0-9])\\.([0-9][0-9][0-9][0-9])\\.([0-9][0-9])([0-9][0-9])([0-9][0-9])([^0-9]|$)")
        # CMake math() reads a leading zero as decimal, so 0104 is 104 and 0905 is 905
        math(EXPR year "${CMAKE_MATCH_1}")
        math(EXPR mmdd "${CMAKE_MATCH_2}")
        math(EXPR hhmm "${CMAKE_MATCH_3}${CMAKE_MATCH_4}")
        math(EXPR ss "${CMAKE_MATCH_5}")
        set(result "${year},${mmdd},${hhmm},${ss}")
    elseif(version MATCHES "^[vV]?([0-9]+)\\.([0-9]+)\\.([0-9]+)(\\.([0-9]+))?([^0-9]|$)")
        set(parts "${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
        # an optional group that did not take part leaves CMAKE_MATCH_5 undefined: compare its value, not its name
        if("${CMAKE_MATCH_5}" STREQUAL "")
            list(APPEND parts 0)
        else()
            list(APPEND parts "${CMAKE_MATCH_5}")
        endif()
        set(fits TRUE)
        set(numbers "")
        foreach(part IN LISTS parts)
            string(LENGTH "${part}" length)
            if(length GREATER 5)
                set(fits FALSE)
            else()
                math(EXPR part "${part}")
                if(part GREATER 65535)
                    set(fits FALSE)
                endif()
                list(APPEND numbers "${part}")
            endif()
        endforeach()
        if(fits)
            string(REPLACE ";" "," result "${numbers}")
        endif()
    endif()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()
