# Collaboration support: wire protocol (shared by client and server) and the standalone server.
# The server only needs QtCore, QtNetwork and QtXml; it does not link the LMMS core.
#
# It can also be built on its own, e.g. on a Linux server (only Qt 6 is needed):
#   cmake -S collab -B build-server -DCMAKE_BUILD_TYPE=Release && cmake --build build-server
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	cmake_minimum_required(VERSION 3.16)
	project(lmms-collab-server CXX)
	set(CMAKE_CXX_STANDARD 20)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	find_package(Qt6 REQUIRED COMPONENTS Core Network Xml)
	set(QT_VERSION_MAJOR 6)
	include(GNUInstallDirs)
	set(BIN_DIR "${CMAKE_INSTALL_BINDIR}")
endif()

add_library(collabprotocol STATIC
	protocol/CollabProtocol.cpp
	protocol/CollabProtocol.h
)
target_include_directories(collabprotocol PUBLIC protocol)
target_link_libraries(collabprotocol PUBLIC Qt${QT_VERSION_MAJOR}::Core)
target_compile_features(collabprotocol PUBLIC cxx_std_20)
set_target_properties(collabprotocol PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(lmms-collab-server
	server/main.cpp
	server/CollabServer.cpp
	server/CollabServer.h
	server/ProjectState.cpp
	server/ProjectState.h
	server/P4Exporter.cpp
	server/P4Exporter.h
)
set_target_properties(lmms-collab-server PROPERTIES AUTOMOC ON)
target_link_libraries(lmms-collab-server PRIVATE
	collabprotocol
	Qt${QT_VERSION_MAJOR}::Core
	Qt${QT_VERSION_MAJOR}::Network
	Qt${QT_VERSION_MAJOR}::Xml
)
install(TARGETS lmms-collab-server RUNTIME DESTINATION "${BIN_DIR}")
if(WIN32)
	# Double-click to run a server for the internet / the local network (see deploy/README.md)
	install(FILES deploy/start-collab-server.bat DESTINATION "${BIN_DIR}")
endif()
