add_executable(midi-ports midi_ports.cpp)
target_link_libraries(midi-ports PRIVATE toonval::core libremidi toonval_warnings)

add_executable(midi-demo midi_demo.cpp)
target_link_libraries(midi-demo PRIVATE libremidi toonval_warnings)

if(TOONVAL_BUILD_APP)
  # Needs SDL3, which only the app option fetches.
  add_executable(display-spike display_spike.cpp)
  target_link_libraries(display-spike PRIVATE SDL3::SDL3 toonval_warnings)
endif()

# Pitch detector evaluation and CPU benchmark: core only, no SDL or MIDI.
add_executable(pitch-eval pitch_eval.cpp)
target_link_libraries(pitch-eval PRIVATE toonval::core toonval_warnings)
