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
