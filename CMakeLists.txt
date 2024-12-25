cmake_minimum_required(VERSION 3.20.0)
project(HomeMonitor VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)

add_executable(HomeMonitor main.cpp)

# Generate Imgui library with Win32 and DX12
add_library(imguiLibrary STATIC)
add_subdirectory(Imgui)
target_link_libraries(HomeMonitor PRIVATE imguiLibrary)

# Add ThingSpeak interface
add_library(thingspeakLibrary STATIC)
add_subdirectory(ThingSpeak)
target_link_libraries(HomeMonitor PRIVATE thingspeakLibrary)

# Add additional resources
add_library(resourcesLibrary OBJECT)
add_subdirectory(Resources)
target_link_libraries(HomeMonitor PRIVATE resourcesLibrary)

target_include_directories(HomeMonitor PRIVATE ${CMAKE_SOURCE_DIR}/include)
