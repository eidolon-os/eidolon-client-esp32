cmake_minimum_required(VERSION 3.16)
project(setup_code_overlay_ui_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# The same host LVGL configuration as the companion UI tests: real renderer,
# QR codes on, snapshots on.
set(LV_BUILD_CONF_PATH "${ROOT}/tests/companion_ui/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/managed_components/lvgl__lvgl" lvgl)
add_executable(setup_code_overlay_ui_tests setup_code_overlay_ui_test.cc
    "${ROOT}/main/display/lvgl_display/setup_code_overlay.cc"
    "${ROOT}/main/eidolon/fonts/font_home_clock_34.c"
    "${ROOT}/main/eidolon/setup_code_card.cc"
    "${ROOT}/main/eidolon/setup_code_secret.cc")
target_include_directories(setup_code_overlay_ui_tests PRIVATE
    "${ROOT}/main" "${ROOT}/main/display" "${ROOT}/main/display/lvgl_display")
target_link_libraries(setup_code_overlay_ui_tests PRIVATE lvgl)
target_compile_options(setup_code_overlay_ui_tests PRIVATE -UNDEBUG)
