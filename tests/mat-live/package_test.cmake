# Staging only: never installs into Program Files or launches capture.
function(verify_runtime runtime)
    file(SHA256 "${BUILD}/sunshine-plugin-mat-capture.exe" expected)
    file(SHA256 "${runtime}/sunshine-plugin-mat-capture.exe" actual)
    if (NOT expected STREQUAL actual OR NOT EXISTS "${runtime}/LICENSE.MinHook.txt")
        message(FATAL_ERROR "Capture publisher or dependency notice missing from package")
    endif ()
    file(READ "${BUILD}/capture_runtime_hashes.h" header)
    foreach(name sunshine-plugin-object-tap sunshine-sonic-mat-worker)
        file(SHA256 "${BUILD}/${name}.dll" expected)
        string(SUBSTRING "${expected}" 0 16 suffix)
        file(SHA256 "${runtime}/${name}-${suffix}.dll" actual)
        if (NOT expected STREQUAL actual OR NOT header MATCHES "${expected}" OR NOT header MATCHES "${name}-${suffix}.dll")
            message(FATAL_ERROR "Installed ${name} does not match the publisher's embedded identity")
        endif ()
    endforeach ()
endfunction ()

string(RANDOM LENGTH 10 ALPHABET 0123456789abcdef suffix)
set(stage "${BUILD}/package-test-${suffix}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DESTDIR
    "${CMAKE_COMMAND}" --install "${BUILD}" --config Release --prefix "${stage}/prefix"
    COMMAND_ERROR_IS_FATAL ANY)
verify_runtime("${stage}/prefix/tools/mat-capture")
# A drive-less prefix also permits testing DESTDIR with native Windows CMake.
# All installed files must stay below this build's private staging directory.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${stage}/relocated"
    "${CMAKE_COMMAND}" --install "${BUILD}" --config Release --prefix /mat-package
    COMMAND_ERROR_IS_FATAL ANY)
verify_runtime("${stage}/relocated/mat-package/tools/mat-capture")
