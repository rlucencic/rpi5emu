# cmake -DIN=<file> -DOUT=<file.c> -DNAME=<symbol> [-DTYPE=<char type>] [-DHEADER=<file.h>] -P embed.cmake
# A file as a C array: const TYPE NAME[] (unsigned char by default, a 0 after
# it), const size_t NAME_size; and with HEADER a header declaring it with
# NAME_SIZE as a constant (for static initialisers: NAME + NAME_SIZE)
if(NOT TYPE)
    set(TYPE "unsigned char")
endif()
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR size "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}"
    "/* Made from ${IN} by sdlemu's cmake/embed.cmake */\n"
    "#include <stddef.h>\n"
    "const ${TYPE} ${NAME}[] = {${bytes}0};\n"
    "const size_t ${NAME}_size = sizeof(${NAME}) - 1;\n")
if(HEADER)
    string(TOUPPER "${NAME}" upper)
    file(WRITE "${HEADER}"
        "/* Made from ${IN} by sdlemu's cmake/embed.cmake */\n"
        "#pragma once\n"
        "#include <stddef.h>\n"
        "#define ${upper}_SIZE ${size}\n"
        "extern const ${TYPE} ${NAME}[];\n"
        "extern const size_t ${NAME}_size;\n")
endif()
