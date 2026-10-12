# The built-in piano engine. It has no SDL in it, so the unit tests link it too and render offline.

# TinySoundFont's implementation plus the few helpers that need its internal structs.
# Third-party code: built without our warning flags.
add_library(toonval_tsf STATIC ${PROJECT_SOURCE_DIR}/app/tsf_glue.cpp)
target_include_directories(toonval_tsf SYSTEM PUBLIC ${tinysoundfont_SOURCE_DIR})
target_compile_features(toonval_tsf PUBLIC cxx_std_20)
set_target_properties(toonval_tsf PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

add_library(toonval_synth STATIC ${PROJECT_SOURCE_DIR}/app/piano_synth.cpp)
target_include_directories(toonval_synth PUBLIC ${PROJECT_SOURCE_DIR}/app)
target_link_libraries(toonval_synth PUBLIC toonval::core PRIVATE toonval_tsf toonval_warnings)
