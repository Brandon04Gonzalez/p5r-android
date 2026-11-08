# Writes OUT (nx_revision.h) with the git revision of SRC, only when it changed (CMakeLists.txt, target nx_revision).
execute_process(COMMAND git -C "${SRC}" rev-parse --short=12 HEAD
                OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT rev)
    set(rev "unknown")
endif()
set(text "#define NX_REVISION \"${rev}\"\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
