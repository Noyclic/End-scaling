cmake_minimum_required(VERSION 3.24)
project(EndScalingNative LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "End-scaling currently targets Windows only.")
endif()

add_library(end_scaling_core STATIC src/d3d11_scaler.cpp src/es_api.cpp)
target_include_directories(end_scaling_core PUBLIC include PRIVATE third_party/amd_fsr1)
target_compile_features(end_scaling_core PUBLIC cxx_std_20)
target_compile_definitions(end_scaling_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(end_scaling_core PUBLIC d3d11 dxgi d3dcompiler)

if(MSVC)
    target_compile_options(end_scaling_core PRIVATE /W4 /permissive- /EHsc)
endif()

include(CTest)
if(BUILD_TESTING)
    # Mirror the packaged layout so relative shader includes resolve identically.
    file(MAKE_DIRECTORY "\${CMAKE_CURRENT_BINARY_DIR}/test-assets")
    file(COPY "\${CMAKE_CURRENT_SOURCE_DIR}/src/shaders"
         DESTINATION "\${CMAKE_CURRENT_BINARY_DIR}/test-assets")
    file(COPY "\${CMAKE_CURRENT_SOURCE_DIR}/third_party/amd_fsr1"
         DESTINATION "\${CMAKE_CURRENT_BINARY_DIR}/test-assets/third_party")

    add_executable(end_scaling_smoke tests/scaler_smoke.cpp)
    target_compile_features(end_scaling_smoke PRIVATE cxx_std_20)
    target_compile_definitions(end_scaling_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(end_scaling_smoke PRIVATE end_scaling_core)
    add_test(NAME d3d11_fsr_shader_compile
        COMMAND end_scaling_smoke "\${CMAKE_CURRENT_BINARY_DIR}/test-assets/shaders/fsr1.hlsl")
endif()
