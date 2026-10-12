add_executable(toonval_tests
  main.cpp
  # App code with no SDL in it: the settings file's atomic save and recovery.
  ${PROJECT_SOURCE_DIR}/app/persistence.cpp
  ${PROJECT_SOURCE_DIR}/app/settings_store.cpp
  # The song controller, library and practice history file: no SDL either.
  ${PROJECT_SOURCE_DIR}/app/song_controller.cpp
  ${PROJECT_SOURCE_DIR}/app/song_library.cpp
  ${PROJECT_SOURCE_DIR}/app/runs_file.cpp
  # The UI's song transport adapter (no SDL).
  ${PROJECT_SOURCE_DIR}/app/song_transport.cpp
  block_label_test.cpp
  chord_display_test.cpp
  chord_test.cpp
  color_test.cpp
  computer_keyboard_test.cpp
  content_hash_test.cpp
  ear_training_test.cpp
  effects_governor_test.cpp
  evaluator_test.cpp
  exercises_test.cpp
  grade_words_test.cpp
  hand_path_test.cpp
  hand_split_test.cpp
  key_state_test.cpp
  keyboard_identity_test.cpp
  keyboard_test.cpp
  keyboard_view_test.cpp
  latency_stats_test.cpp
  library_test.cpp
  look_color_test.cpp
  look_names_test.cpp
  look_serialize_test.cpp
  look_test.cpp
  midi_event_queue_test.cpp
  midi_event_test.cpp
  midi_file_test.cpp
  midi_stream_test.cpp
  midi_writer_test.cpp
  musicxml_fuzz_test.cpp
  musicxml_real_test.cpp
  musicxml_test.cpp
  name_fit_test.cpp
  note_style_test.cpp
  note_trail_test.cpp
  note_window_test.cpp
  octave_numbering_test.cpp
  octave_shift_test.cpp
  persistence_test.cpp
  piano_synth_test.cpp
  pitch_accuracy_test.cpp
  pitch_detect_test.cpp
  pitch_eval_test.cpp
  pitch_expected_test.cpp
  pitch_realtime_test.cpp
  pitch_test.cpp
  playback_test.cpp
  pointer_keys_test.cpp
  practice_plan_test.cpp
  progress_daily_test.cpp
  progress_history_test.cpp
  progress_trouble_test.cpp
  recorder_test.cpp
  run_record_test.cpp
  runs_file_test.cpp
  scales_test.cpp
  screen_layout_test.cpp
  section_suggestions_test.cpp
  sections_fuzz_test.cpp
  sections_property_test.cpp
  sections_store_test.cpp
  sections_test.cpp
  session_restore_test.cpp
  session_state_test.cpp
  settings_doc_test.cpp
  settings_fuzz_test.cpp
  settings_store_test.cpp
  song_builder_test.cpp
  song_controller_test.cpp
  song_test.cpp
  song_transport_test.cpp
  sound_sink_test.cpp
  spsc_queue_test.cpp
  transport_test.cpp
  transpose_test.cpp
  user_presets_test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(toonval_tests PRIVATE toonval::core toonval_synth doctest::doctest
  Threads::Threads toonval_warnings)
# The synth tests also run on the real piano samples when the build has them.
if(TOONVAL_PIANO_SF2)
  target_compile_definitions(toonval_tests PRIVATE TOONVAL_TEST_PIANO_SF2="${TOONVAL_PIANO_SF2}")
endif()
target_include_directories(toonval_tests PRIVATE ${PROJECT_SOURCE_DIR}/app)
add_test(NAME toonval_tests COMMAND toonval_tests)
target_compile_definitions(toonval_tests PRIVATE
  TOONVAL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
