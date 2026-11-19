cmake_minimum_required(VERSION 3.19)
project(js8mesh VERSION 1.0.1 LANGUAGES CXX)

# Qt 6.4 or later (Linux Mint 22 ships 6.4.2), or Qt 5.15 for 32-bit Windows (Qt 6 has no
# 32-bit Windows builds).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(MSVC)
    add_compile_options(/utf-8 /W3)   # sources are UTF-8 (status symbols, JS8's end mark)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
if(QT_VERSION_MAJOR EQUAL 6)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets Network Bluetooth SerialPort)
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core Widgets Network Bluetooth SerialPort)
endif()
set(QT Qt${QT_VERSION_MAJOR})

# everything but the window, so the tests can use it
add_library(js8mesh_core STATIC
    src/Settings.h src/Settings.cpp
    src/MeshProto.h src/MeshProto.cpp
    src/MeshLink.h src/MeshLink.cpp
    src/BleMeshLink.h src/BleMeshLink.cpp
    src/StreamMeshLink.h src/StreamMeshLink.cpp
    src/Js8Api.h src/QtCompat.h
    src/SubspaceRemoteApi.h src/SubspaceRemoteApi.cpp
    src/Gateway.h src/Gateway.cpp
)
target_include_directories(js8mesh_core PUBLIC src)
target_link_libraries(js8mesh_core PUBLIC ${QT}::Core ${QT}::Network ${QT}::Bluetooth ${QT}::SerialPort)

add_executable(js8mesh WIN32 src/main.cpp src/MainWindow.h src/MainWindow.cpp)
target_compile_definitions(js8mesh PRIVATE JS8MESH_VERSION="${PROJECT_VERSION}")
target_link_libraries(js8mesh PRIVATE js8mesh_core ${QT}::Widgets)

include(CTest)
if(BUILD_TESTING)
    add_executable(meshproto_test tests/meshproto_test.cpp)
    target_link_libraries(meshproto_test PRIVATE js8mesh_core)
    add_test(NAME meshproto COMMAND meshproto_test)
    add_executable(gateway_test tests/gateway_test.cpp)
    target_link_libraries(gateway_test PRIVATE js8mesh_core)
    add_test(NAME gateway COMMAND gateway_test)
endif()

install(TARGETS js8mesh RUNTIME DESTINATION bin)
if(UNIX AND NOT APPLE)
    install(FILES packaging/js8mesh.desktop DESTINATION share/applications)
endif()

# Linux: a .deb for Linux Mint (cpack -G DEB)
set(CPACK_PACKAGE_NAME "js8mesh")
set(CPACK_PACKAGE_VENDOR "WM8Q")
set(CPACK_PACKAGE_CONTACT "https://github.com/avantol/Subspace-Meshtastic-Basic")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Subspace Edition (JS8) <-> Meshtastic gateway")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# the platform plugin (window system) and BlueZ (Bluetooth) aren't found by shlibdeps
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins, bluez")
set(CPACK_DEBIAN_PACKAGE_SECTION "hamradio")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
include(CPack)
