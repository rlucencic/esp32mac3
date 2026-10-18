# M5Stack Tab5 only (target esp32p4: idf.py -B build-tab5 -DIDF_TARGET=esp32p4
# -DSDKCONFIG=build-tab5/sdkconfig ...). The MaTouch's ESP32-S3 is too slow
# for a 68030; ../esp32mac is the Mac for that board.
cmake_minimum_required(VERSION 3.16)

# The shared board runtime and the 68000 core: ../esp32emu
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32emu/components")

# The Tab5: the board's settings from ../esp32emu after this project's
if(IDF_TARGET STREQUAL "esp32p4" OR "$ENV{IDF_TARGET}" STREQUAL "esp32p4")
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
                           "${CMAKE_CURRENT_LIST_DIR}/../esp32emu/boards/m5stack_tab5/sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "esp32mac3 runs on the M5Stack Tab5 only: "
        "idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-tab5/sdkconfig build. "
        "The MaTouch's ESP32-S3 is too slow for a 68030 (../esp32mac runs there).")
endif()
idf_build_set_property(MINIMAL_BUILD ON)
if(IDF_TARGET STREQUAL "esp32p4")
    # its own components (the BSP, esp_hosted, ...): its own lock file
    idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.esp32p4")
endif()
project(esp32mac3)

# Macintosh ROM images (not in git) go to their own partitions on "idf.py
# flash": the LC II's and the LC III's
foreach(rom lc2 lc3)
    set(rom_image "${CMAKE_SOURCE_DIR}/roms/${rom}.rom")
    if(EXISTS "${rom_image}")
        esptool_py_flash_to_partition(flash "rom_${rom}" "${rom_image}")
    endif()
endforeach()
