# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "1.0.0")
project(eidolon)

# Enforce the storage contract for every Hub board, including direct idf.py builds.
if(CONFIG_EIDOLON_HUB_MODE)
    # Enforce the SDK source for board scripts AND direct idf.py builds. Keeping
    # a matching remote commit in managed_components does not include local edits.
    idf_component_get_property(EIDOLON_LIVEKIT_COMPONENT_DIR livekit COMPONENT_DIR)
    get_filename_component(EIDOLON_LIVEKIT_EXPECTED_DIR
        "${CMAKE_SOURCE_DIR}/../vendor/client-sdk-esp32/components/livekit" REALPATH)
    get_filename_component(EIDOLON_LIVEKIT_ACTUAL_DIR
        "${EIDOLON_LIVEKIT_COMPONENT_DIR}" REALPATH)
    if(NOT EIDOLON_LIVEKIT_ACTUAL_DIR STREQUAL EIDOLON_LIVEKIT_EXPECTED_DIR)
        message(FATAL_ERROR "LiveKit must compile from local vendor: ${EIDOLON_LIVEKIT_EXPECTED_DIR}; resolved: ${EIDOLON_LIVEKIT_ACTUAL_DIR}")
    endif()
    idf_build_get_property(EIDOLON_PYTHON PYTHON)
    string(REPLACE "MB" "" EIDOLON_FLASH_MIB "${CONFIG_ESPTOOLPY_FLASHSIZE}")
    math(EXPR EIDOLON_FLASH_BYTES "${EIDOLON_FLASH_MIB} * 1024 * 1024")
    execute_process(
        COMMAND "${EIDOLON_PYTHON}" "${CMAKE_SOURCE_DIR}/scripts/eidolon/partition_contract.py"
            csv "${PARTITION_CSV_PATH}"
            --offset "${CONFIG_PARTITION_TABLE_OFFSET}"
            --flash-bytes "${EIDOLON_FLASH_BYTES}"
        RESULT_VARIABLE EIDOLON_STORAGE_RESULT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/scripts/eidolon/partition_contract.py"
        "${CMAKE_SOURCE_DIR}/main/eidolon/owner_trust_storage_policy.h")
    if(NOT EIDOLON_STORAGE_RESULT EQUAL 0)
        message(FATAL_ERROR "Eidolon storage contract failed; fix the board partition configuration")
    endif()
    add_custom_target(eidolon_partition_contract ALL
        COMMAND "${EIDOLON_PYTHON}" "${CMAKE_SOURCE_DIR}/scripts/eidolon/partition_contract.py"
            build "${CMAKE_BINARY_DIR}"
        DEPENDS app partition_table_bin bootloader)
    if(TARGET generated_default_assets)
        add_dependencies(eidolon_partition_contract generated_default_assets)
    endif()
endif()
