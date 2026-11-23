# The script is to generate ELF for application

# This file's directory. Inside project_elf(), CMAKE_CURRENT_LIST_DIR is the
# calling app's directory, so capture the loader's own location here.
set(ELF_LOADER_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Where the headers, libraries and tools are. This file is also the app SDK's
# viper_sdk.cmake (scripts/export_sdk.py copies it unchanged), whose folder
# has a VERSION file and its own layout; in the viper repo they're found from
# components/elf_loader/.
if(EXISTS "${ELF_LOADER_DIR}/VERSION")
    set(VIPER_SDK_INCLUDE_DIR "${ELF_LOADER_DIR}/include")
    set(VIPER_SDK_LIBS_DIR "${ELF_LOADER_DIR}/libs")
    set(VIPER_SDK_LVGL_DIR "${ELF_LOADER_DIR}/lvgl")
    set(VIPER_SDK_TOOL_DIR "${ELF_LOADER_DIR}/tools")
    # Apps link against the firmware's C++ runtime by mangled name
    # (exports/cxx.txt), so they must use the firmware's compiler: the same
    # ESP-IDF major.minor.
    file(STRINGS "${ELF_LOADER_DIR}/VERSION" _viper_sdk_idf REGEX "^esp-idf ")
    string(REGEX REPLACE "^esp-idf +" "" _viper_sdk_idf "${_viper_sdk_idf}")
    if(DEFINED IDF_VERSION_MAJOR AND
       NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" STREQUAL "${_viper_sdk_idf}")
        message(FATAL_ERROR "This Viper SDK needs ESP-IDF ${_viper_sdk_idf}, "
            "not ${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}: apps use the firmware's C++ runtime "
            "and must be built with the same compiler")
    endif()
else()
    set(VIPER_SDK_INCLUDE_DIR "${ELF_LOADER_DIR}/sdk/include")
    set(VIPER_SDK_LIBS_DIR "${ELF_LOADER_DIR}/..")
    set(VIPER_SDK_LVGL_DIR "${ELF_LOADER_DIR}/../../third_party/lvgl")
    set(VIPER_SDK_TOOL_DIR "${ELF_LOADER_DIR}/tool")
endif()

# Trick to temporarily redefine project(). When functions are overridden in CMake, the originals can still be accessed
# using an underscore prefixed function of the same name. The following lines make sure that project  calls
# the original project(). See https://cmake.org/pipermail/cmake/2015-October/061751.html.
function(project_elf)
endfunction()

function(_project_elf)
endfunction()

# The compile and link flags, strip flags and checker every app build uses.
# Sets cflags, strip_flags, CMAKE_LINKER, MODULE_CHECK_LD, python and
# check_app_elf in the calling scope; a macro, so project_elf() and
# project_elfs() share them.
macro(_viper_elf_setup)
    # Enable these options to remove unused symbols and reduce linked objects
    # -fPIC: Generate position-independent code (required for dynamic loading)
    #        This ensures code uses GOT (Global Offset Table) for accessing
    #        global data and strings, making it relocatable.
    set(cflags -nostartfiles
               -nostdlib
               -fPIC
               -shared
               -e app_main
               -fdata-sections
               -ffunction-sections
               -Wl,--gc-sections
               -fvisibility=hidden)

    # Don't let the linker relax long calls into direct PC-relative calls.
    # The loader copies .shared_iram away from .text, into shared memory, so a
    # direct call8 between the two (which only works where they're adjacent in
    # the file) jumps into whatever follows .text. Long calls go through a
    # literal the loader relocates.
    list(APPEND cflags -Wl,--no-relax)

    # Apps run from PSRAM, where the S3's atomic compare-and-swap (S32C1I)
    # doesn't work. Like the firmware (ESP-IDF's
    # CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND), compile atomics into calls
    # to the firmware's __atomic_* functions (exports/cxx.txt) instead.
    list(APPEND cflags -mdisable-hardware-atomics)

    # The app SDK's headers (viper_app.h)
    list(APPEND cflags -I${VIPER_SDK_INCLUDE_DIR})

    # Game libraries the app asked for with set(VIPER_LIBS ...) before
    # project_elf(): platform-free code shared with built-in apps and the sim
    # (docs/plans/game_api_consolidation.md). This adds their include
    # folders; the sim reads the same line (sim/CMakeLists.txt). All but wfc
    # and drum_synth are header-only: an app using one of those lists its
    # src/*.cpp in its own SRCS (apps/screensavers/circuit/CMakeLists.txt,
    # apps/drum_machine/main/CMakeLists.txt), and the sim links the firmware's
    # copy.
    set(viper_known_libs gfx game sfx espgl wfc drum_synth raycast)
    foreach(lib IN LISTS VIPER_LIBS)
        if(NOT lib IN_LIST viper_known_libs)
            message(FATAL_ERROR "VIPER_LIBS: unknown library '${lib}' (known: ${viper_known_libs})")
        endif()
        list(APPEND cflags -I${VIPER_SDK_LIBS_DIR}/${lib}/include)
    endforeach()

    # The firmware's LVGL headers, configured like the firmware: LVGL reads its
    # CONFIG_LV_* values from the SDK's viper_lvgl_config.h instead of the
    # app's own sdkconfig.h, which has none. Apps call the firmware's LVGL
    # (exports/lvgl.txt); they don't link their own.
    list(APPEND cflags -I${VIPER_SDK_LVGL_DIR})
    idf_build_set_property(COMPILE_DEFINITIONS
        "LV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"viper_lvgl_config.h\"" APPEND)

    # Linker flags for better PIC support
    # -z relro: Make relocations read-only after loading (helps with PIC)
    # -z now: Resolve all symbols at load time (better for dynamic loading)
    # Note: These flags may not be supported on all toolchains, but are standard ELF
    list(APPEND cflags -Wl,-z,relro
                       -Wl,-z,now)

    # Enable this options to remove unnecessary sections in
    list(APPEND cflags -Wl,--strip-all
                       -Wl,--strip-debug
                       -Wl,--strip-discarded)

    list(APPEND cflags -Dmain=app_main)

    idf_build_set_property(COMPILE_OPTIONS "${cflags}" APPEND)

    # Remove more unused sections
    # NOTE: We preserve .got, .got.plt, and .dynamic as they are needed for
    # proper position-independent code and dynamic symbol resolution.
    # Removing .dynamic can break string access from .dynstr section.
    string(REPLACE "-elf-gcc" "-elf-strip" ${CMAKE_STRIP} ${CMAKE_C_COMPILER})
    set(strip_flags --strip-unneeded
                    --remove-section=.comment
                    --remove-section=.got.loc)
                    # Removed: --remove-section=.dynamic (needed for dynamic linking)
                    # Removed: --remove-section=.got (needed for PIC data access)
                    # Removed: --remove-section=.got.plt (needed for PIC function calls)

    if(CONFIG_IDF_TARGET_ARCH_XTENSA)
        list(APPEND strip_flags --remove-section=.xt.lit
                                --remove-section=.xt.prop
                                --remove-section=.xtensa.info)
    elseif(CONFIG_IDF_TARGET_ARCH_RISCV)
        list(APPEND strip_flags --remove-section=.riscv.attributes)
    endif()

    # Get the linker path (replace gcc with ld)
    string(REPLACE "-gcc" "-ld" CMAKE_LINKER ${CMAKE_C_COMPILER})
    
    # Linker script for the module check below
    set(MODULE_CHECK_LD "${ELF_LOADER_DIR}/module_check.ld")

    # Checks the ELF against the loader's rules (sections, relocation types,
    # exported symbols); see tool/check_app_elf.py
    idf_build_get_property(python PYTHON)
    set(check_app_elf "${VIPER_SDK_TOOL_DIR}/check_app_elf.py")
endmacro()

# Links the static libraries `libs` (with libgcc) into `elf_app`, strips it,
# and checks that the loader can load it. `output` names the custom command's
# output (it's never created, so the link runs on every build); `deps` are the
# targets that build the libraries.
macro(_viper_elf_link output elf_app libs deps)
    set(_viper_link_libs ${libs})
    # libgcc: the compiler's helpers for what the chip can't do in one
    # instruction (float division, 64-bit conversions), linked into the app
    # like any compiler's output. -nostdlib leaves it out otherwise.
    list(APPEND _viper_link_libs -lgcc)
    get_filename_component(_viper_elf_stem ${elf_app} NAME_WE)
    set(module_check_elf "${CMAKE_BINARY_DIR}/${_viper_elf_stem}.module_check.elf")
    set(module_check_map "${CMAKE_BINARY_DIR}/${_viper_elf_stem}.module_check.map")
    add_custom_command(OUTPUT ${output}
        COMMAND ${CMAKE_C_COMPILER} ${cflags} ${_viper_link_libs} -o ${elf_app}
        COMMAND ${CMAKE_STRIP} ${strip_flags} ${elf_app}
        # Run linker check to validate ELF can be relocated
        # This validates that the ELF structure is compatible with the loader
        # The -r flag creates a relocatable object, validating the ELF can be processed
        COMMAND ${CMAKE_LINKER}
            -T ${MODULE_CHECK_LD}
            -Map=${module_check_map}
            -nostdlib
            --unresolved-symbols=ignore-all
            -o ${module_check_elf}
            ${elf_app}
        COMMAND ${python} ${check_app_elf} ${elf_app}
        DEPENDS ${deps} ${MODULE_CHECK_LD} ${check_app_elf}
        COMMENT "Build ELF: ${elf_app} and check that the loader can load it"
        )
endmacro()

# One app: the project's main component (and any ELF_COMPONENTS and ELF_LIBS)
# linked into <project>.app.elf
macro(project_elf project_name)
    _viper_elf_setup()
    set(elf_app "${CMAKE_PROJECT_NAME}.app.elf")

    # Link input list of libraries to ELF
    list(PREPEND ELF_COMPONENTS "main")
    if(ELF_COMPONENTS)
        foreach(c ${ELF_COMPONENTS})
            list(APPEND elf_libs "esp-idf/${c}/lib${c}.a")

            if(${CMAKE_GENERATOR} STREQUAL "Unix Makefiles")
                add_custom_command(OUTPUT elf_${c}_app
                        COMMAND +${CMAKE_MAKE_PROGRAM} "__idf_${c}/fast"
                        COMMENT "Build Component: ${c}"
                )
                list(APPEND elf_dependeces "elf_${c}_app")
            else()
                list(APPEND elf_dependeces "idf::${c}")
            endif()
        endforeach()
    endif()
    if (ELF_LIBS)
        list(APPEND elf_libs "${ELF_LIBS}")
    endif()
    spaces2list(elf_libs)

    _viper_elf_link(elf_app ${elf_app} "${elf_libs}" "${elf_dependeces}")
    add_custom_target(elf ALL DEPENDS elf_app)
endmacro()

# Several apps from one project: each name is a component of the project (a
# folder in EXTRA_COMPONENT_DIRS), linked on its own into <name>.app.elf. For
# families of small apps, such as apps/screensavers, that would otherwise each
# need a project of their own. Each component defines its own app_main()
# (VIPER_APP()), so they can't share a library; shared code goes in headers.
macro(project_elfs)
    _viper_elf_setup()
    set(_viper_elf_outputs "")
    foreach(_viper_elf_name ${ARGN})
        _viper_elf_link(elf_${_viper_elf_name}_app ${_viper_elf_name}.app.elf
            "esp-idf/${_viper_elf_name}/lib${_viper_elf_name}.a" "idf::${_viper_elf_name}")
        list(APPEND _viper_elf_outputs elf_${_viper_elf_name}_app)
    endforeach()
    add_custom_target(elf ALL DEPENDS ${_viper_elf_outputs})
endmacro()
