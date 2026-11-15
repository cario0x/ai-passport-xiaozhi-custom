set(companion_dir "${CMAKE_BINARY_DIR}/companion")
file(MAKE_DIRECTORY "${companion_dir}")
if(PASSPORT_MINIAPP)
    set(face_pack_option --miniapp)
    set(face_decoder "src/compact_face.c")
endif()
execute_process(COMMAND "${PYTHON}" "${PROJECT_DIR}/tools/companion_pack.py"
    --output "${companion_dir}" ${face_pack_option} RESULT_VARIABLE companion_result)
if(NOT companion_result EQUAL 0)
    message(FATAL_ERROR "Companion resource generation failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_DIR}/tools/companion_pack.py" "src/lv_font_cn_16.c" "src/lv_font_cn_24.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_DIR}/tools/body_font_pack.py" "${PROJECT_DIR}/tools/compact_faces.py" "${PROJECT_DIR}/assets/fonts/hangul16.bin" "${PROJECT_DIR}/assets/fonts/hangul20.bin")
file(GLOB companion_avatars "${PROJECT_DIR}/assets/xiaozhi/faces/*.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${companion_avatars})
# components/presentation/CMakeLists.txt
idf_component_register(
    SRCS "src/ui_profile.c" "src/ui_album.c" "src/ui_game.c" "src/ui_reward.c"
         "src/ui_factory.c" "src/ui_selftest.c" "src/ui_button_guide.c"
         "src/ui_theme.c" "src/ui_official.c" "src/ui_boot.c" "src/ui_xiaozhi.c" "src/ui_plays.c" "src/bobo_art.c"
         "src/ui_font_body.c" "src/ui_font_title.c" "src/ui_font_answer.c"
         "src/ui_font_caption.c" "src/ui_font_book.c"
         "src/ui_licenses.c" "src/font_notices.c"
         "src/hangul_font.c" "src/passport_font_blocks.c" "vendor/puff/puff.c" "src/pixel_sprite.c" "src/pet_art_pack.c"
         "${companion_dir}/lv_font_cn_16.c" "${companion_dir}/lv_font_cn_24.c" "src/companion_resources.c" ${face_decoder}
    EMBED_FILES "${PROJECT_DIR}/assets/fonts/languages.bin"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${companion_dir}"
    REQUIRES services lvgl xiaozhi platform_esp32
    PRIV_REQUIRES esp_rom esp_partition)
