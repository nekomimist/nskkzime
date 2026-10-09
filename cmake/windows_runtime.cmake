  add_library(nskkzime_runtime STATIC src/windows/dictionary_settings.cpp src/windows/runtime_dictionary.cpp src/windows/romaji_settings.cpp
    src/windows/learning_ipc.cpp src/windows/learning_client.cpp src/windows/dictionary_cache.cpp src/windows/tool_launcher.cpp src/windows/input_timing.cpp src/windows/cpu_requirements.cpp
    src/windows/app_container.cpp src/windows/container_runtime.cpp src/windows/broker_snapshot.cpp src/windows/runtime_configuration.cpp src/windows/diagnostic_file.cpp src/windows/tsf_diagnostics.cpp)
  target_include_directories(nskkzime_runtime PUBLIC src/windows)
  target_include_directories(nskkzime_runtime SYSTEM PUBLIC third_party)
  target_link_libraries(nskkzime_runtime PUBLIC skk_dictionary nskkzime_protocol shell32 ole32 uuid advapi32 bcrypt userenv)
  add_library(nskkzime_inference STATIC src/windows/inference_ipc.cpp src/windows/inference_client.cpp src/windows/input_log.cpp)
  target_link_libraries(nskkzime_inference PUBLIC nskkzime_runtime nskkzime_protocol user32)
  add_library(nskkzime_tsf_support STATIC src/windows/tsf_edit.cpp src/windows/composition_session.cpp src/windows/inference_session.cpp)
  target_link_libraries(nskkzime_tsf_support PUBLIC nskkzime_inference)
  add_library(nskkzime_tsf SHARED src/windows/text_service.cpp src/windows/module.cpp
    src/windows/registration.cpp src/windows/registration_registry.cpp src/windows/display_attributes.cpp
    src/windows/candidate_ui.cpp src/windows/input_mode.cpp src/windows/context_text.cpp src/windows/nskkzime.def)
  target_include_directories(nskkzime_tsf PRIVATE src/windows)
  target_link_libraries(nskkzime_tsf PRIVATE nskkzime_tsf_support ole32 oleaut32 advapi32 uuid user32 gdi32)
