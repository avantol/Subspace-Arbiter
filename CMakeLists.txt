cmake_minimum_required(VERSION 3.22)
project(Arbiter VERSION 1.0.1 LANGUAGES CXX)

# The version string the program shows and the packages carry. CI passes the
# tag (v1.0.0 -> 1.0.0) or a branch build (1.0.0-dev.<sha>); a plain build
# uses the project version.
set(ARBITER_VERSION_STRING "${PROJECT_VERSION}" CACHE STRING "version shown by the program")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
if(NOT MSVC)
  add_compile_options(-Wall -Wextra)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Sql Widgets)

configure_file(src/config.h.in ${CMAKE_BINARY_DIR}/config.h @ONLY)

set(SOURCES
  src/main.cpp
  src/Settings.cpp
  src/Log.cpp
  src/XmlRpcClient.cpp
  src/FlrigWatch.cpp
  src/FldigiLink.cpp
  src/SubspaceLink.cpp
  src/VaracWatch.cpp
  src/VaracLever.cpp
  src/Arbiter.cpp
  src/MainWindow.cpp
  src/Settings.h
  src/Log.h
  src/XmlRpcClient.h
  src/FlrigWatch.h
  src/FldigiLink.h
  src/SubspaceLink.h
  src/VaracWatch.h
  src/VaracLever.h
  src/Arbiter.h
  src/MainWindow.h
)

# WIN32: a GUI-subsystem program on Windows (no console window); no effect elsewhere.
qt_add_executable(Arbiter WIN32 ${SOURCES})
if(WIN32)
  target_sources(Arbiter PRIVATE icons/arbiter.rc)
  set_target_properties(Arbiter PROPERTIES OUTPUT_NAME "Arbiter")
else()
  set_target_properties(Arbiter PROPERTIES OUTPUT_NAME "arbiter")
endif()
target_include_directories(Arbiter PRIVATE ${CMAKE_BINARY_DIR} src)
target_link_libraries(Arbiter PRIVATE Qt::Core Qt::Network Qt::Sql Qt::Widgets)

# Windows: gather the exe and its Qt DLLs/plugins into build/Arbiter/, the
# folder the installer packs (the same shape Subspace Edition uses).
if(WIN32)
  get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
  add_custom_command(TARGET Arbiter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Arbiter"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:Arbiter>" "${CMAKE_BINARY_DIR}/Arbiter/"
    COMMAND "${_qt_bin}/windeployqt" --no-translations --no-opengl-sw --no-system-d3d-compiler
            "${CMAKE_BINARY_DIR}/Arbiter/Arbiter.exe"
    COMMENT "windeployqt into build/Arbiter/")
endif()

# Linux install rules (the .deb is built from these by packaging/build-deb.sh)
if(UNIX AND NOT APPLE)
  include(GNUInstallDirs)
  install(TARGETS Arbiter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES packaging/deb/arbiter.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES icons/arbiter.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
endif()
