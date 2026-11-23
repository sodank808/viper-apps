# nethack/ is NetHack 5.0.0 (its include/ and src/, and the regex matcher
# from sys/share), and lua/ the Lua 5.4.8 it bundles; port/ is NetHack's
# window port and platform for Viper; the rest is the app around them. See
# ../README.md. The simulator reads INCLUDE_DIRS from here too.
idf_component_register(SRC_DIRS "." "port" "nethack/src" "nethack/sys/share" "lua"
                       INCLUDE_DIRS "." "port" "nethack/include" "lua")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -DNDEBUG # the firmware doesn't export __assert_func
    $<$<COMPILE_LANGUAGE:C>:-std=gnu99>
    # One section a file, not a function: with a section a function, the
    # linker's --gc-sections (which every app links with) trips an assertion
    # in binutils' Xtensa code on an app this big (elf32-xtensa.c, binutils
    # 2.43). NetHack uses nearly all of its code anyway.
    -fno-function-sections
    -fno-data-sections)

# NetHack's and Lua's warnings are upstream's, left as they have them
file(GLOB upstream_srcs ${CMAKE_CURRENT_SOURCE_DIR}/nethack/src/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/nethack/sys/share/*.c ${CMAKE_CURRENT_SOURCE_DIR}/lua/*.c)
set_source_files_properties(${upstream_srcs} PROPERTIES COMPILE_OPTIONS "-w")

# glyphs.c's glyph loops: GCC's induction variables (-fivopts) start a
# pointer far before the table it walks (table - first glyph's offset),
# which the ELF loader can't relocate: it maps pointers only into the
# sections it loads (check_app_elf.py says so at build time)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/nethack/src/glyphs.c
    PROPERTIES COMPILE_OPTIONS "-w;-fno-ivopts")
