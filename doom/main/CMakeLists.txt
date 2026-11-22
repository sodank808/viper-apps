# prboom/ is the engine (PrBoom by way of esp-cpp/prboom, esp-box-emu's
# copy), the rest is the app around it. See ../README.md.
idf_component_register(SRC_DIRS "." "prboom"
                       INCLUDE_DIRS "." "prboom")

# The engine is speed-bound (the renderer's inner loops), so it's built for
# speed over the ELF apps' default -Os; these come after it, so they win.
# The warnings are the engine's own, left as upstream has them.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -DNDEBUG # midifile.c's one assert; the firmware doesn't export __assert_func
    -Wno-misleading-indentation
    -Wno-format-overflow
    -Wno-format-truncation
    -Wno-char-subscripts
    -Wno-missing-field-initializers
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-error
    $<$<COMPILE_LANGUAGE:C>:-std=gnu17>
    $<$<COMPILE_LANGUAGE:C>:-Wno-discarded-qualifiers>
    $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
    $<$<COMPILE_LANGUAGE:C>:-Wno-pointer-sign>
    $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-function-declaration>)
