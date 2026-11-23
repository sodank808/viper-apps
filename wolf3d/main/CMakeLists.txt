# engine/ is Wolf4SDL (the portable port of id Software's source), with
# Viper's platform in place of SDL; the rest is the app around it. See
# ../README.md.
idf_component_register(SRC_DIRS "." "engine" "engine/dosbox"
                       INCLUDE_DIRS "." "engine")

# The engine is speed-bound (the renderer's inner loops), so it's built for
# speed over the ELF apps' default -Os; this comes after it, so it wins.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -DNDEBUG # the engine's asserts; the firmware doesn't export __assert_func
    $<$<COMPILE_LANGUAGE:C>:-std=gnu17>)

# The engine's warnings are upstream's, left as it has them
file(GLOB engine_srcs ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/engine/dosbox/*.cpp)
set_source_files_properties(${engine_srcs} PROPERTIES COMPILE_OPTIONS "-w")
