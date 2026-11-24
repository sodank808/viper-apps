# The apps' host simulator tests, for the Viper firmware repo's sim build,
# which includes this file when this repo is checked out at apps_public/.
# The simulator is part of the firmware repo, so these only build and run
# there; this file is ignored anywhere else.
#
# VIPER_PUBLIC_APPS is this folder. The sim already compiles every app here
# (each folder with an info.json) and picks up each app's boot tests by name
# (<app>/tests/test_<app>_boot.cpp). List here what it can't find by name:
#
#   target_sources(viper_sim_app_tests PRIVATE              # unit tests, and
#       ${VIPER_PUBLIC_APPS}/<app>/tests/test_<x>.cpp       # the plain C++
#       ${VIPER_PUBLIC_APPS}/<app>/main/<x>.cpp)            # they test
#   target_include_directories(viper_sim_app_tests PRIVATE ${VIPER_PUBLIC_APPS}/<app>/main)
#   target_include_directories(viper_sim_boot_deps INTERFACE ${VIPER_PUBLIC_APPS}/<app>/main)

# Wolfenstein 3D's app (wolf3d_app.h), whose status its boot tests read
target_include_directories(viper_sim_boot_deps INTERFACE ${VIPER_PUBLIC_APPS}/wolf3d/main)

# NetHack: its data (nhdat), packed for the tests by its own tools from
# nethack/upstream/, and its app and engine headers for its boot tests
set(NETHACK_SIM_DATA ${CMAKE_CURRENT_BINARY_DIR}/nethack_data)
file(GLOB nethack_dat CONFIGURE_DEPENDS ${VIPER_PUBLIC_APPS}/nethack/upstream/dat/*
     ${VIPER_PUBLIC_APPS}/nethack/upstream/util/*)
add_custom_command(OUTPUT ${NETHACK_SIM_DATA}/nhdat
    COMMAND ${VIPER_PUBLIC_APPS}/nethack/tools/make_nhdat.sh ${NETHACK_SIM_DATA}
    DEPENDS ${VIPER_PUBLIC_APPS}/nethack/tools/make_nhdat.sh ${nethack_dat}
    COMMENT "Packing NetHack's data (nhdat)")
# and its tiles (tiles.bin)
file(GLOB nethack_tiles CONFIGURE_DEPENDS ${VIPER_PUBLIC_APPS}/nethack/upstream/win/share/*.txt)
add_custom_command(OUTPUT ${NETHACK_SIM_DATA}/tiles.bin
    COMMAND python3 -I ${VIPER_PUBLIC_APPS}/nethack/tools/make_tiles.py ${NETHACK_SIM_DATA}
    DEPENDS ${VIPER_PUBLIC_APPS}/nethack/tools/make_tiles.py ${nethack_tiles}
            ${VIPER_PUBLIC_APPS}/nethack/main/nethack/src/tile.c
    COMMENT "Packing NetHack's tiles (tiles.bin)")
add_custom_target(nethack_sim_data DEPENDS ${NETHACK_SIM_DATA}/nhdat ${NETHACK_SIM_DATA}/tiles.bin)
add_dependencies(viper_sim_app_boot_tests nethack_sim_data)
target_compile_definitions(viper_sim_app_boot_tests PRIVATE
    VIPER_SIM_NETHACK_DATA="${NETHACK_SIM_DATA}")
target_include_directories(viper_sim_boot_deps INTERFACE ${VIPER_PUBLIC_APPS}/nethack/main)
target_sources(viper_sim_app_tests PRIVATE ${VIPER_PUBLIC_APPS}/nethack/tests/test_nethack_model.cpp
    ${VIPER_PUBLIC_APPS}/nethack/main/nethack_view.cpp
    ${VIPER_PUBLIC_APPS}/nethack/main/nethack_tiles.cpp
    ${VIPER_PUBLIC_APPS}/nethack/main/nethack_first_person.cpp)
add_dependencies(viper_sim_app_tests nethack_sim_data)
target_compile_definitions(viper_sim_app_tests PRIVATE
    VIPER_SIM_NETHACK_DATA="${NETHACK_SIM_DATA}")
target_include_directories(viper_sim_app_tests PRIVATE ${VIPER_PUBLIC_APPS}/nethack/main
    ${VIPER_ROOT}/components/raycast/include)

# Genesis: the machine (genesis_core.cpp and gwenesis) for its unit tests,
# with the test ROMs' facts (smoke_md.h) from the firmware repo's sim/tests
file(GLOB genesis_gwenesis_srcs CONFIGURE_DEPENDS ${VIPER_PUBLIC_APPS}/genesis/main/gwenesis/*.c)
if(VIPER_SIM_SANITIZE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    # gwenesis reads emulated memory through 16- and 32-bit pointers at any
    # even address, as nofrendo does (the firmware repo's sim/CMakeLists.txt
    # does the same for the copy compiled into the simulator)
    set_source_files_properties(${genesis_gwenesis_srcs} PROPERTIES
        COMPILE_OPTIONS -fno-sanitize=alignment)
endif()
target_sources(viper_sim_app_tests PRIVATE ${VIPER_PUBLIC_APPS}/genesis/tests/test_genesis.cpp
    ${VIPER_PUBLIC_APPS}/genesis/main/genesis_core.cpp ${genesis_gwenesis_srcs})
target_include_directories(viper_sim_app_tests PRIVATE ${VIPER_PUBLIC_APPS}/genesis/main
    ${VIPER_ROOT}/sim/tests/roms/genesis)
