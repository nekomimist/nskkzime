# Refresh at build time, including direct cmake --build and incremental builds.
find_program(NSKKZIME_PWSH NAMES pwsh REQUIRED)
set(_metadata_source "${PROJECT_BINARY_DIR}/build-metadata/$<CONFIG>/build_metadata.cpp")
set(_metadata_variant "test-only")
set(_metadata_llama_args)
if(NSKKZIME_WITH_ZENZ)
  set(_metadata_variant "zenz-cpu")
  list(APPEND _metadata_llama_args -LlamaDirectory "${NSKKZIME_LLAMA_DIR}" -LlamaRevision "${NSKKZIME_LLAMA_REVISION}")
endif()
add_custom_target(nskkzime_build_metadata
  COMMAND "${NSKKZIME_PWSH}" -NoProfile -File "${PROJECT_SOURCE_DIR}/scripts/write-build-metadata.ps1"
    -Repository "${PROJECT_SOURCE_DIR}" -Output "${_metadata_source}" -Configuration "$<CONFIG>"
    -CompilerVersion "${CMAKE_CXX_COMPILER_VERSION}" -CacheFile "${PROJECT_BINARY_DIR}/CMakeCache.txt"
    -Variant "${_metadata_variant}" ${_metadata_llama_args}
  BYPRODUCTS "${_metadata_source}"
  VERBATIM)
foreach(_target nskkzime_tsf nskkzime_dictionary_server nskkzime_settings nskkzime_dictionary_gui
    nskkzime_config nskkzime_user_dictionary ime_probe nkkz-ime-server)
  target_sources(${_target} PRIVATE "${_metadata_source}")
  target_compile_definitions(${_target} PRIVATE "NSKKZIME_BINARY_NAME=\"$<TARGET_FILE_NAME:${_target}>\"")
  add_dependencies(${_target} nskkzime_build_metadata)
endforeach()
