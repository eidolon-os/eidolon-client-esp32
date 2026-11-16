cmake_minimum_required(VERSION 3.16)
project(pet_panel_ui_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# Same host LVGL build as tests/companion_ui (16-bit colour, software renderer).
set(LV_BUILD_CONF_PATH "${ROOT}/tests/companion_ui/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/managed_components/lvgl__lvgl" lvgl)
set(UI "${ROOT}/main/eidolon/third_party/smooth_ui_toolkit/src")
file(GLOB_RECURSE UI_CORE "${UI}/core/*.cpp")
file(GLOB SKIN "${ROOT}/main/eidolon/avatar/skins/default/*.cpp")
# The production view built as StackChan builds it (desk-pet face and control
# panel); only the pet's body, the audio levels, chirp playback, the sense queue
# and the display lock are replaced.
add_executable(pet_panel_ui_tests pet_panel_ui_test.cc ${UI_CORE} ${SKIN}
    "${ROOT}/main/eidolon/avatar/skins/default/assets/default_bubble_arrow.c"
    "${ROOT}/main/eidolon/views/companion_face_view.cc"
    "${ROOT}/main/eidolon/views/pet_face_skin.cc"
    "${ROOT}/main/eidolon/pet/pet_runtime.cc"
    "${ROOT}/main/eidolon/pet/pet_sense.cc"
    "${ROOT}/main/eidolon/eidolon_view.cc"
    "${ROOT}/main/eidolon/expression/core/runtime.cc"
    "${ROOT}/main/eidolon/output_policy.cc"
    "${ROOT}/managed_components/espressif__cjson/cJSON/cJSON.c"
    "${ROOT}/managed_components/78__xiaozhi-fonts/src/font_awesome_20_4.c"
    "${ROOT}/managed_components/78__xiaozhi-fonts/src/font_awesome_30_4.c"
    "${ROOT}/managed_components/78__xiaozhi-fonts/src/font_puhui_basic_20_4.c")
target_include_directories(pet_panel_ui_tests PRIVATE stubs "${ROOT}/tests/companion_ui/stubs"
    "${ROOT}/main" "${UI}" "${UI}/lvgl"
    "${ROOT}/managed_components/espressif__cjson/cJSON"
    "${ROOT}/managed_components/78__xiaozhi-fonts/include")
target_compile_definitions(pet_panel_ui_tests PRIVATE CONFIG_EIDOLON_COMPANION_FACE=1 CONFIG_EIDOLON_PET_FACE=1
    CONFIG_EIDOLON_PET_SESSION_BUTTON=1 CONFIG_EIDOLON_CAP_EXPRESSION=1 CONFIG_EIDOLON_CAP_AUDIO_CUE=1
    CONFIG_EIDOLON_CAP_MOTION=1)
target_link_libraries(pet_panel_ui_tests PRIVATE lvgl)
target_compile_options(pet_panel_ui_tests PRIVATE -UNDEBUG)
