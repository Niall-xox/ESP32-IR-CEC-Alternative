cmake_minimum_required(VERSION 3.16)

# The version lives in one file, read by everything that needs it: this build,
# the Nix package, and the release tag. It used to be spelled out separately in
# each, which is a drift waiting to happen — a package claiming a version it
# was not built from is a bug nobody notices until they are chasing one.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DAEMON_VERSION LIMIT_COUNT 1)

project(esp32-ir-remote-daemon VERSION ${DAEMON_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single-config generators (Makefiles, Ninja) default to an empty build type,
# which produces an unoptimised binary.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Warnings are not on by default at any build type or optimisation level. The
# tree is clean under these; making that the enforced baseline is what keeps it
# that way, rather than depending on whoever last compiled it having checked.
# Not -Werror: a future compiler inventing a new warning should not be able to
# stop a user building a release they need.
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
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
    list(APPEND SOURCES src/WindowsPowerMonitor.cpp src/WindowsPowerCapabilities.cpp)
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
    # PowrProf carries CallNtPowerInformation, which reports whether this
    # machine is S3 or Modern Standby and whether Fast Startup is on. It ships
    # with every Windows SDK under a name that has not changed, so it is linked
    # normally rather than loaded by hand — unlike the power-setting and
    # device-interface GUIDs, whose defining libraries do vary by SDK version
    # and are spelled out in the sources instead.
    #
    # Everything else the Windows build calls — the service API,
    # RegisterPowerSettingNotification, RegisterDeviceNotification, the file
    # and environment functions — resolves from the default MSVC link set.
    #
    # The Win32 API surface this uses — SERVICE_ACCEPT_PRESHUTDOWN,
    # RegisterPowerSettingNotification, HPOWERNOTIFY, HDEVNOTIFY — is Vista and
    # later. Naming the version rather than inheriting whatever the SDK happens
    # to default to keeps a toolchain upgrade from silently changing what
    # compiles.
    target_link_libraries(esp32-ir-daemon PRIVATE hidapi::winapi PowrProf)
    target_compile_definitions(esp32-ir-daemon PRIVATE
        _WIN32_WINNT=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
else()
    target_link_libraries(esp32-ir-daemon PRIVATE PkgConfig::HIDAPI)
endif()

# The Windows monitor runs its worker on a std::thread.
find_package(Threads REQUIRED)
target_link_libraries(esp32-ir-daemon PRIVATE Threads::Threads)

if(UNIX AND NOT APPLE)
    target_link_libraries(esp32-ir-daemon PRIVATE SDBusCpp::sdbus-c++)
endif()

# -----------------------------------------------------------------------------
# Install
#
# Everything below is Linux-only: the Windows build is installed by its own
# installer, which registers the service with `sc create` rather than dropping
# a unit file. Guarding the whole block keeps the Windows configure step clean.
# -----------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)

    # The unit file's ExecStart must match wherever the binary actually lands,
    # which differs between a hand `cmake --install` (/usr/local) and a package
    # (/usr). configure_file resolves it at configure time.
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/esp32-ir-remote.service.in
        ${CMAKE_CURRENT_BINARY_DIR}/esp32-ir-remote.service
        @ONLY
    )

    install(TARGETS esp32-ir-daemon
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Two deliberate choices in these two destinations:
    #
    # Literal `lib`, not ${CMAKE_INSTALL_LIBDIR}. GNUInstallDirs resolves LIBDIR
    # to lib64 on Fedora and openSUSE, but neither udev nor systemd ever looks
    # in lib64 — the files would install successfully and then simply never be
    # read. Both search paths are literal `lib` on every distribution.
    #
    # Relative, not ${CMAKE_INSTALL_PREFIX}/lib/... . A relative destination is
    # rebased onto whatever prefix is in effect at install time, which is how
    # CPack stages a package: it re-runs the install with the prefix pointed at
    # a temporary directory. An absolute path ignores that and writes to the
    # real /usr on the build machine — CPack fails outright rather than
    # producing a package, which is at least a loud failure.
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/99-esp32-ir-remote.rules
            DESTINATION lib/udev/rules.d)

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/esp32-ir-remote.service
            DESTINATION lib/systemd/system)
endif()

# -----------------------------------------------------------------------------
# Packaging (CPack)
#
#   cpack -G DEB --config daemon/build/CPackConfig.cmake
#   cpack -G RPM --config daemon/build/CPackConfig.cmake
#
# DEB shells out to dpkg-deb and RPM to rpmbuild, so each can only be produced
# on a host that has that tool — see .github/workflows/packages.yml, which
# builds both in containers. Arch is not covered here; it uses the separate
# PKGBUILD in packaging/.
#
# These packages are intended for the GitHub releases page. They would not pass
# Debian or Fedora archive review, which wants debhelper and .spec sources
# rather than CPack output.
# -----------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    set(CPACK_PACKAGE_NAME "esp32-ir-remote")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Mirrors PC power state to a TV over IR, as a CEC alternative")
    set(CPACK_PACKAGE_HOMEPAGE_URL
        "https://github.com/Niall-xox/ESP32-IR-CEC-Alternative")
    set(CPACK_PACKAGE_CONTACT "Niall <niall.fatah2003@gmail.com>")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_STRIP_FILES ON)

    # Package names differ between distributions, so the dependency string has
    # to be spelled twice. Both demand sdbus-c++ 2.x — a system carrying 1.x
    # now fails at install time with a legible dependency error instead of
    # producing a daemon that cannot start.
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsdbus-c++2 (>= 2.0), libhidapi-hidraw0")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/prerm")

    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_REQUIRES "sdbus-cpp >= 2.0, hidapi")
    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/postinst")
    set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/prerm")

    # /usr/lib/systemd and /usr/lib/udev are owned by the systemd package.
    # Claiming them here makes rpm report a file conflict on install.
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/usr/lib/systemd;/usr/lib/systemd/system;/usr/lib/udev;/usr/lib/udev/rules.d")

    include(CPack)
endif()
