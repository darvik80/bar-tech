# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON CACHE BOOL "" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ESP-IDF 6 / GCC 15: embed-framework triggers these as -Werror.
# Keep as warnings, do not fail the build.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-result" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=missing-field-initializers" APPEND)

# Suppress ESP-IDF internal component warnings
idf_build_set_property(COMPILE_OPTIONS "-w" APPEND)

project(bar-face)

# Ensure srmodels.bin is generated during normal builds (not just flash target).
# esp-sr only creates srmodels_bin when the flash target is built; CLion never
# invokes flash, so without this dependency the model partition gets flashed
# with stale/empty data → dl_nn_assert crash at runtime.
cmake_language(DEFER CALL add_dependencies bar-face.elf srmodels_bin)
