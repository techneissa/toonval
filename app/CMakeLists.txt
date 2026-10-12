include(${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake)
set(font_source ${CMAKE_CURRENT_BINARY_DIR}/embedded_font.cpp)
toonval_embed_file(${TOONVAL_FONT_FILE} embedded_font ${font_source})
set(font_regular_source ${CMAKE_CURRENT_BINARY_DIR}/embedded_font_regular.cpp)
toonval_embed_file(${TOONVAL_FONT_REGULAR_FILE} embedded_font_regular ${font_regular_source})
set(icons_source ${CMAKE_CURRENT_BINARY_DIR}/embedded_icons.cpp)
toonval_embed_file(${TOONVAL_ICON_FONT_FILE} embedded_icons ${icons_source})
set(icon_256_source ${CMAKE_CURRENT_BINARY_DIR}/embedded_icon_256.cpp)
toonval_embed_file(${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/toonval-window-256.png embedded_icon_256
                   ${icon_256_source})
set(icon_64_source ${CMAKE_CURRENT_BINARY_DIR}/embedded_icon_64.cpp)
toonval_embed_file(${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/toonval-window-64.png embedded_icon_64
                   ${icon_64_source})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TOONVAL_FONT_FILE}
                                       ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/toonval-window-256.png
                                       ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/toonval-window-64.png
                                                               ${TOONVAL_FONT_REGULAR_FILE}
                                                               ${TOONVAL_ICON_FONT_FILE})

# Dear ImGui with its SDL3 and SDLRenderer3 backends. Third-party code: no warnings of ours.
add_library(toonval_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(toonval_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR}
                                                       ${imgui_SOURCE_DIR}/backends)
target_compile_features(toonval_imgui PUBLIC cxx_std_20)
target_link_libraries(toonval_imgui PUBLIC SDL3::SDL3)
set_target_properties(toonval_imgui PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

add_executable(toonval main.cpp midi_input.cpp midi_output.cpp virtual_input.cpp mesh.cpp scene.cpp
  text.cpp settings_store.cpp persistence.cpp dev_script.cpp effects.cpp bench.cpp audio_out.cpp
  synth_test.cpp song_controller.cpp song_library.cpp runs_file.cpp song_transport.cpp
  ui/host.cpp ui/kit.cpp ui/pages.cpp ui/sheet.cpp ui/bar_strip.cpp ui/bar_groups.cpp
  ui/bar_dock.cpp ui/transport.cpp ui/preview.cpp window_icon.cpp)
target_link_libraries(toonval PRIVATE toonval::core toonval_synth SDL3::SDL3 SDL3_ttf::SDL3_ttf
  libremidi toonval_imgui toonval_warnings)
# The repository's test songs join the library in every build from a checkout (the folder is
# skipped when it is not there).
target_compile_definitions(toonval PRIVATE TOONVAL_DEV_SONGS_DIR="${PROJECT_SOURCE_DIR}/tests/data")
if(APPLE)
  target_link_libraries(toonval PRIVATE "-framework CoreAudio")
endif()
# The piano samples go beside the executable, where AudioOut looks for them.
if(TOONVAL_PIANO_SF2)
  configure_file(${TOONVAL_PIANO_SF2} ${CMAKE_CURRENT_BINARY_DIR}/toonval-piano.sf2 COPYONLY)
endif()
# Generated, so kept out of the warning flags.
add_library(toonval_font OBJECT ${font_source} ${font_regular_source} ${icons_source}
  ${icon_256_source} ${icon_64_source})
target_compile_features(toonval_font PRIVATE cxx_std_20)
target_sources(toonval PRIVATE $<TARGET_OBJECTS:toonval_font>)
