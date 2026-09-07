cmake_minimum_required(VERSION 3.16)
project(attention_ui_tests C)
set(CMAKE_C_STANDARD 11)
set(LVGL_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../managed_components/lvgl__lvgl" CACHE PATH "LVGL 9.5.0 source")
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_SOURCE_DIR}" lvgl)
add_executable(attention_ui_test attention_ui_test.c ../main/display_frame.c)
target_include_directories(attention_ui_test PRIVATE stubs ../main)
target_link_libraries(attention_ui_test PRIVATE lvgl m)
enable_testing()
add_test(NAME attention_ui COMMAND attention_ui_test)
add_executable(display_frame_test display_frame_test.c ../main/display_frame.c)
target_include_directories(display_frame_test PRIVATE ../main)
add_test(NAME display_frame COMMAND display_frame_test)

add_executable(voice_control_test voice_control_test.c ../main/voice_control.c)
target_include_directories(voice_control_test PRIVATE stubs ../main)
add_test(NAME voice_control COMMAND voice_control_test)

add_executable(wireless_microphone_protocol_test
    wireless_microphone_protocol_test.c
    ../main/wireless_microphone_protocol.c
)
target_include_directories(wireless_microphone_protocol_test PRIVATE ../main)
add_test(NAME wireless_microphone_protocol COMMAND wireless_microphone_protocol_test)

# Exercise the real capture implementation without ESP-IDF or hardware.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME voice_audio_capture
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/run_voice_audio_tests.py")

add_test(NAME wireless_microphone_lifecycle
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/run_wireless_lifecycle_tests.py")
