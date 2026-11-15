# cmake -DIN=<file> -DOUT=<file.c> -DNAME=<symbol> -P embed.cmake
# A file as a C array: const unsigned char NAME[] (with a 0 after it) and
# const size_t NAME_size
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}"
    "/* Made from ${IN} by rpi5emu's cmake/embed.cmake */\n"
    "#include <stddef.h>\n"
    "const unsigned char ${NAME}[] = {${bytes}0};\n"
    "const size_t ${NAME}_size = sizeof(${NAME}) - 1;\n")
