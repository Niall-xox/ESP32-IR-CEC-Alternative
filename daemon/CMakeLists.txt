cmake_minimum_required(VERSION 3.16)
project(esp32-ir-remote-daemon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single-config generators (Makefiles, Ninja) default to an empty build type,
# which produces an unoptimised binary with no warnings enabled.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Sources compiled on all platforms
set(SOURCES
    src/main.cpp
    src/HIDTransport.cpp
)

# Platform-specific sources and dependencies.
if(UNIX AND NOT APPLE)
    # Linux: D-Bus power events via systemd-logind.
    #
    # Version 2.0 is a hard requirement — LinuxPowerMonitor uses the v2 API
    # (sdbus::ServiceName / sdbus::ObjectPath strong typedefs, and signal
    # registration without finishRegistration()). Without the version here,
    # find_package succeeds against a v1 install and the build then fails much
    # later with unreadable template errors. Asking for the version makes it
    # fail immediately with "found version 1.x, required 2.0".
    list(APPEND SOURCES src/LinuxPowerMonitor.cpp)
    find_package(sdbus-c++ 2.0 REQUIRED)
elseif(WIN32)
    # Windows: power events via the Win32 Service API
    list(APPEND SOURCES src/WindowsPowerMonitor.cpp)
endif()

# hidapi handles USB HID communication on all platforms.
# The backend differs per OS (hidraw on Linux, Win32 HID API on Windows)
# but the API is identical — HIDTransport.cpp is unchanged on both.
if(WIN32)
    # hidapi is provided via vcpkg. Build with:
    #   cmake -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
    #         -DVCPKG_TARGET_TRIPLET=x64-windows
    # vcpkg provides hidapi::winapi (Win32 HID backend) and hidapi::include.
    # PkgConfig is not used on Windows — it is not available there.
    find_package(hidapi CONFIG REQUIRED)

    # vcpkg places headers in .../include/hidapi/, but HIDTransport.h includes
    # "hidapi/hidapi.h", which requires the parent .../include/ on the path.
    # Derive it from the target property rather than hardcoding a path.
    get_target_property(_hidapi_inc hidapi::include INTERFACE_INCLUDE_DIRECTORIES)
    cmake_path(GET _hidapi_inc PARENT_PATH HIDAPI_INCLUDE_DIRS)
else()
    # IMPORTED_TARGET builds PkgConfig::HIDAPI, which carries the include dirs,
    # library dirs and compile flags together. Linking the bare ${HIDAPI_LIBRARIES}
    # name list drops the library search path, which fails to link on distros
    # that keep hidapi outside the default linker paths.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HIDAPI REQUIRED IMPORTED_TARGET hidapi-hidraw)
endif()

add_executable(esp32-ir-daemon ${SOURCES})
target_include_directories(esp32-ir-daemon PRIVATE src ${HIDAPI_INCLUDE_DIRS})

if(WIN32)
    target_link_libraries(esp32-ir-daemon PRIVATE hidapi::winapi)
else()
    target_link_libraries(esp32-ir-daemon PRIVATE PkgConfig::HIDAPI)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(esp32-ir-daemon PRIVATE SDBusCpp::sdbus-c++)
endif()
