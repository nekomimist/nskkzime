# Release-only prototype following Microsoft's two-pass CMake ARM64X build.
if(NOT MSVC OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR
    NOT CMAKE_CONFIGURATION_TYPES STREQUAL "Release" OR
    NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreadedDLL")
  message(FATAL_ERROR "ARM64X requires MSVC, Release only, and /MD.")
endif()
set(_arm64x_root "${PROJECT_BINARY_DIR}/arm64x/Release")
file(MAKE_DIRECTORY "${_arm64x_root}")
set(_identity "{")
foreach(_key CMAKE_CXX_COMPILER CMAKE_CXX_COMPILER_ID CMAKE_CXX_COMPILER_VERSION CMAKE_VS_PLATFORM_TOOLSET
    CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION CMAKE_MSVC_RUNTIME_LIBRARY CMAKE_CXX_STANDARD
    CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_RELEASE CMAKE_GENERATOR_INSTANCE CMAKE_GENERATOR_PLATFORM)
  set(_value "${${_key}}")
  string(REPLACE "\\" "\\\\" _value "${_value}")
  string(REPLACE "\"" "\\\"" _value "${_value}")
  string(APPEND _identity "\"${_key}\":\"${_value}\",")
endforeach()
string(APPEND _identity "\"scope\":\"tsf-only\",\"configuration\":\"$<CONFIG>\"}")
file(GENERATE OUTPUT "${_arm64x_root}/build-config.json" CONTENT "${_identity}\n")
if(NSKKZIME_ARM64X_MODE STREQUAL "NATIVE" AND CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
  target_link_options(nskkzime_tsf PRIVATE "/LINKREPROFULLPATHRSP:${_arm64x_root}/native-link.rsp")
elseif(NSKKZIME_ARM64X_MODE STREQUAL "EC" AND CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64EC")
  # CMake initializes this shared-link flag for EC. It must not override the
  # ARM64X target machine selected below. Executables/static libs remain EC.
  string(REPLACE "/machine:ARM64EC" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
  option(NSKKZIME_ARM64X_TRACE_LINK "Trace ARM64X linker library selection" OFF)
  set(NSKKZIME_ARM64X_NATIVE_DIRECTORY "${PROJECT_SOURCE_DIR}/out/build/windows-arm64x-native" CACHE PATH "Matching ARM64X native inputs")
  set(_inputs "${NSKKZIME_ARM64X_NATIVE_DIRECTORY}/arm64x/Release/native-inputs.cmake")
  if(NOT EXISTS "${_inputs}")
    message(FATAL_ERROR "Run scripts/build-arm64x.ps1 to prepare matching native inputs.")
  endif()
  include("${_inputs}")
  set_source_files_properties(${NSKKZIME_ARM64X_NATIVE_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  target_sources(nskkzime_tsf PRIVATE ${NSKKZIME_ARM64X_NATIVE_OBJECTS})
  target_link_libraries(nskkzime_tsf PRIVATE ${NSKKZIME_ARM64X_NATIVE_LIBRARIES})
  target_link_options(nskkzime_tsf PRIVATE /MACHINE:ARM64X "/DEFARM64NATIVE:${NSKKZIME_ARM64X_NATIVE_DEF}")
  if(NSKKZIME_ARM64X_TRACE_LINK)
    target_link_options(nskkzime_tsf PRIVATE /VERBOSE:LIB)
  endif()
  add_custom_target(nskkzime_arm64x_check
    COMMAND "${NSKKZIME_PWSH}" -NoProfile -File "${PROJECT_SOURCE_DIR}/scripts/prepare-arm64x-inputs.ps1"
      -NativeDirectory "${NSKKZIME_ARM64X_NATIVE_DIRECTORY}" -EcDirectory "${PROJECT_BINARY_DIR}" -Verify
    VERBATIM)
  add_dependencies(nskkzime_build_metadata nskkzime_arm64x_check)
else()
  message(FATAL_ERROR "Invalid ARM64X build mode/platform.")
endif()
