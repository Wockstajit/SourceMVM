cmake_minimum_required(VERSION 3.24)
project(SourceMvmRenderDocBridge LANGUAGES CXX)

if(NOT RENDERDOC_SOURCE_DIR)
  message(FATAL_ERROR "Set RENDERDOC_SOURCE_DIR to the official RenderDoc checkout")
endif()

set(RENDERDOC_APP_HEADER "${RENDERDOC_SOURCE_DIR}/renderdoc/api/app/renderdoc_app.h")
if(NOT EXISTS "${RENDERDOC_APP_HEADER}")
  message(FATAL_ERROR "Missing official RenderDoc application header: ${RENDERDOC_APP_HEADER}")
endif()

add_library(SourceMvmRenderDocBridge SHARED RenderDocBridge.cpp)
target_compile_features(SourceMvmRenderDocBridge PRIVATE cxx_std_17)
target_compile_definitions(SourceMvmRenderDocBridge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(SourceMvmRenderDocBridge PRIVATE "${RENDERDOC_SOURCE_DIR}/renderdoc/api/app")
target_link_libraries(SourceMvmRenderDocBridge PRIVATE Advapi32)
