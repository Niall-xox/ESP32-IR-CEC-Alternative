cmake_minimum_required(VERSION 3.16)
project(esp32-ir-remote-daemon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sources compiled on all platforms
set(SOURCES
    src/main.cpp
    src/SerialTransport.cpp
)

# Linux-specific sources and dependencies.
# Windows sources and their dependencies will be added here in Phase 2
# under a corresponding if(WIN32) block.
if(UNIX AND NOT APPLE)
    list(APPEND SOURCES src/LinuxPowerMonitor.cpp)
    find_package(sdbus-c++ REQUIRED)
endif()

add_executable(esp32-ir-daemon ${SOURCES})
target_include_directories(esp32-ir-daemon PRIVATE src)

if(UNIX AND NOT APPLE)
    target_link_libraries(esp32-ir-daemon PRIVATE SDBusCpp::sdbus-c++)
endif()
