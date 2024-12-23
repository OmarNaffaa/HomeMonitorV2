cmake_minimum_required(VERSION 3.20.0)
project(HomeMonitor VERSION 0.1.0 LANGUAGES C CXX)

add_executable(HomeMonitor main.cpp)

# Generate Imgui library with Win32 and DX12
add_library(imguiLibrary)
add_subdirectory(Imgui)

# Add library and build project
target_link_libraries(HomeMonitor PRIVATE imguiLibrary)
target_include_directories(HomeMonitor PRIVATE ${CMAKE_SOURCE_DIR}/include)
