cmake_minimum_required(VERSION 3.16)
project(pet_face_render_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# Same host LVGL build as tests/companion_ui (16-bit colour, software renderer).
set(LV_BUILD_CONF_PATH "${ROOT}/tests/companion_ui/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/managed_components/lvgl__lvgl" lvgl)
add_executable(pet_face_render_tests pet_face_render_test.cc
    "${ROOT}/main/eidolon/views/pet_face_skin.cc"
    "${ROOT}/main/eidolon/pet/pet_runtime.cc"
    "${ROOT}/main/eidolon/pet/pet_sense.cc"
    "${ROOT}/managed_components/78__xiaozhi-fonts/src/font_puhui_basic_20_4.c")
target_include_directories(pet_face_render_tests PRIVATE "${ROOT}/tests/companion_ui/stubs" "${ROOT}/main"
    "${ROOT}/managed_components/78__xiaozhi-fonts/include")
target_link_libraries(pet_face_render_tests PRIVATE lvgl)
target_compile_options(pet_face_render_tests PRIVATE -UNDEBUG)
