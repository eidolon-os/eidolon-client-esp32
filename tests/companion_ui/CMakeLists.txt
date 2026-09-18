cmake_minimum_required(VERSION 3.16)
project(companion_ui_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/managed_components/lvgl__lvgl" lvgl)
set(UI "${ROOT}/main/eidolon/third_party/smooth_ui_toolkit/src")
file(GLOB_RECURSE UI_CORE "${UI}/core/*.cpp")
file(GLOB SKIN "${ROOT}/main/eidolon/avatar/skins/default/*.cpp")
add_executable(companion_ui_tests companion_ui_test.cc ${UI_CORE} ${SKIN}
    "${ROOT}/main/eidolon/avatar/skins/default/assets/default_bubble_arrow.c"
    "${ROOT}/main/eidolon/views/companion_face_view.cc"
    "${ROOT}/main/eidolon/eidolon_view.cc"
    "${ROOT}/main/eidolon/expression/core/runtime.cc"
    "${ROOT}/main/eidolon/output_policy.cc"
    "${ROOT}/managed_components/espressif__cjson/cJSON/cJSON.c"
    "${ROOT}/managed_components/78__xiaozhi-fonts/src/font_awesome_20_4.c"
    "${ROOT}/managed_components/78__xiaozhi-fonts/src/font_noto_basic_20_4.c"
    "${ROOT}/managed_components/78__xiaozhi-fonts/src/font_puhui_basic_20_4.c")
target_include_directories(companion_ui_tests PRIVATE stubs "${ROOT}/main" "${UI}" "${UI}/lvgl"
    "${ROOT}/managed_components/espressif__cjson/cJSON")
target_compile_definitions(companion_ui_tests PRIVATE CONFIG_EIDOLON_COMPANION_FACE=1 CONFIG_EIDOLON_CAP_EXPRESSION=1 CONFIG_EIDOLON_CAP_DIALOGUE_TEXT=1)
target_link_libraries(companion_ui_tests PRIVATE lvgl)
# Keep assertions active even when optimizing the renderer for fast screenshots.
target_compile_options(companion_ui_tests PRIVATE -UNDEBUG)
