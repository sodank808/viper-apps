# gwenesis/ is the Genesis core (esp-cpp/gwenesis, vendored; every change is
# marked "Viper:"), genesis_core.cpp the machine around it, main.cpp the app.
# See ../README.md.
idf_component_register(SRC_DIRS "." "gwenesis"
                       INCLUDE_DIRS "." "gwenesis")

# The core is speed-bound and its sources ask for -Ofast themselves (#pragma
# GCC optimize); the app around it is built for size like other ELF apps. The
# warnings are the core's own, left as upstream has them.
target_compile_options(${COMPONENT_LIB} PRIVATE
    # ESP-IDF turns jump tables off for every component (for code in IRAM,
    # where a table in flash would be a problem; an app's code and rodata
    # both run from PSRAM). The Z80's opcode switch was a chain of compares
    # for every instruction; the GBA core found the same (its DESIGN.md).
    -fjump-tables
    -ftree-switch-conversion
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-unused-value
    -Wno-unused-const-variable
    -Wno-misleading-indentation
    -Wno-implicit-fallthrough
    -Wno-char-subscripts
    -Wno-error
    $<$<COMPILE_LANGUAGE:C>:-Wno-discarded-qualifiers>
    $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
    $<$<COMPILE_LANGUAGE:C>:-Wno-pointer-sign>)
