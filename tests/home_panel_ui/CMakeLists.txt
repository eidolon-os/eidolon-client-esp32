cmake_minimum_required(VERSION 3.16)
project(home_panel_ui_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(SDK_GOLDEN "${ROOT}/../eidolon_sdk/contracts/smarthome/v1/golden" ABSOLUTE)
set(LV_BUILD_CONF_PATH "${ROOT}/tests/companion_ui/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/managed_components/lvgl__lvgl" lvgl)
add_executable(home_panel_ui_tests home_panel_ui_test.cc
    "${ROOT}/main/eidolon/views/home_panel_view.cc"
    "${ROOT}/main/eidolon/views/home_panel_copy.cc"
    "${ROOT}/main/eidolon/eidolon_view.cc"
    "${ROOT}/main/eidolon/ui_state_mapper.cc"
    "${ROOT}/main/eidolon/eidolon_ui_labels.cc"
    "${ROOT}/main/eidolon/smarthome/smarthome_store.cc"
    "${ROOT}/main/eidolon/smarthome/smarthome_tiles.cc"
    "${ROOT}/main/eidolon/smarthome/smarthome_wire.cc"
    "${ROOT}/main/eidolon/fonts/font_home_icons_20.c"
    "${ROOT}/main/eidolon/fonts/font_home_icons_24.c"
    "${ROOT}/main/eidolon/fonts/font_home_icons_48.c"
    "${ROOT}/main/eidolon/fonts/font_home_text_20.c"
    "${ROOT}/main/eidolon/fonts/font_home_clock_34.c"
    "${ROOT}/managed_components/espressif__cjson/cJSON/cJSON.c"
    "${ROOT}/managed_components/78__xiaozhi-fonts/src/font_awesome_20_4.c"
    "${ROOT}/managed_components/78__xiaozhi-fonts/src/font_puhui_basic_20_4.c")
target_include_directories(home_panel_ui_tests PRIVATE stubs "${ROOT}/tests/companion_ui/stubs"
    "${ROOT}/tests/companion_ui" "${ROOT}/main"
    "${ROOT}/managed_components/espressif__cjson/cJSON"
    "${ROOT}/managed_components/78__xiaozhi-fonts/include")
# korvo-1's interaction mode (main/boards/korvo-1/config.json).
target_compile_definitions(home_panel_ui_tests PRIVATE CONFIG_EIDOLON_INTERACTION_MODE_HALF_DUPLEX=1
    SDK_GOLDEN_DIR="${SDK_GOLDEN}"
    CBIN_FONT_FILE="${ROOT}/managed_components/78__xiaozhi-fonts/cbin/font_puhui_common_20_4.bin")
target_link_libraries(home_panel_ui_tests PRIVATE lvgl)
# Keep assertions active even when optimizing the renderer for fast screenshots.
target_compile_options(home_panel_ui_tests PRIVATE -UNDEBUG)
