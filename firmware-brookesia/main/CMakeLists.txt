# 你的原有内容
file(GLOB_RECURSE MAIN_SRCS ./*.cpp ./*.c)
set(EMBEDDED_FILES "")

if (CONFIG_EXAMPLE_COZE_AGENT_PRIVATE_KEY_FILE_PATH)
    list(APPEND EMBEDDED_FILES ${CONFIG_EXAMPLE_COZE_AGENT_PRIVATE_KEY_FILE_PATH})
    message(STATUS "Coze Private Key Path: ${CONFIG_EXAMPLE_COZE_AGENT_PRIVATE_KEY_FILE_PATH}")
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    EMBED_TXTFILES ${EMBEDDED_FILES}
    REQUIRES Settings system_status XiaozhiApp brookesia_app_vibe vibe_wifi
)

target_compile_options(${COMPONENT_LIB} PUBLIC -Wno-missing-field-initializers)


# ======= 把函数定义也一起加进来（必须在第一次调用前） =======
function(get_component_library component_name output_var)
    # Get the exact component name
    idf_build_get_property(build_components BUILD_COMPONENTS)
    set(TARGET_COMPONENT "")
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${component_name}" OR COMPONENT MATCHES "espressif__${component_name}")
            set(TARGET_COMPONENT ${COMPONENT})
            break()
        endif()
    endforeach()

    # Get the component library
    if(TARGET_COMPONENT STREQUAL "")
        message(FATAL_ERROR "Component '${component_name}' not found.")
    else()
        idf_component_get_property(COMPONENT_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
        set(${output_var} ${COMPONENT_LIB} PARENT_SCOPE)
    endif()
endfunction()

# 现在可以安全调用 get_component_library 了
get_component_library("gmf_core" GMF_CORE_LIB)
target_compile_options(
    ${GMF_CORE_LIB}
    PUBLIC
        "-fpermissive"
)

# XiaoZhi needs the complete Chinese glyph set at runtime. Keep the 2.4 MB
# file in the 4 MB assets SPIFFS partition rather than in the app image.
idf_build_get_property(build_components BUILD_COMPONENTS)
set(XIAOZHI_FONTS_DIR "")
foreach(component IN LISTS build_components)
    if(component MATCHES "xiaozhi-fonts")
        idf_component_get_property(XIAOZHI_FONTS_DIR ${component} COMPONENT_DIR)
        break()
    endif()
endforeach()
if(NOT XIAOZHI_FONTS_DIR)
    message(FATAL_ERROR "xiaozhi-fonts component was not resolved")
endif()

set(XIAOZHI_COMMON_FONT
    "${XIAOZHI_FONTS_DIR}/cbin/font_puhui_common_30_4.bin")
if(NOT EXISTS "${XIAOZHI_COMMON_FONT}")
    message(FATAL_ERROR "xiaozhi-fonts is missing font_puhui_common_30_4.bin")
endif()

set(ASSETS_STAGING_DIR "${CMAKE_BINARY_DIR}/assets_spiffs")
set(ASSETS_STAGING_STAMP "${CMAKE_BINARY_DIR}/assets_spiffs.stamp")
add_custom_command(
    OUTPUT "${ASSETS_STAGING_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${ASSETS_STAGING_DIR}/xiaozhi"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${XIAOZHI_COMMON_FONT}"
        "${ASSETS_STAGING_DIR}/xiaozhi/font.bin"
    COMMAND "${CMAKE_COMMAND}" -E touch "${ASSETS_STAGING_STAMP}"
    DEPENDS "${XIAOZHI_COMMON_FONT}"
    COMMENT "Staging XiaoZhi Chinese font for SPIFFS"
    VERBATIM
)
add_custom_target(assets_spiffs_stage DEPENDS "${ASSETS_STAGING_STAMP}")
spiffs_create_partition_image(
    assets "${ASSETS_STAGING_DIR}"
    FLASH_IN_PROJECT
    DEPENDS assets_spiffs_stage
)
