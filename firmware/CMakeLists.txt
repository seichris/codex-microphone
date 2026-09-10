cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(codex_esp32_display)

# UAC's supported composite mode: one stack and PHY for audio plus CDC.
idf_component_get_property(uac_dir espressif__usb_device_uac COMPONENT_DIR)
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/main/usb"
    "${uac_dir}/tusb_uac")
target_sources(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/main/usb/usb_descriptors.c")
