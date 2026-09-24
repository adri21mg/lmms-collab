# Collaboration support: wire protocol (shared by client and server) and the standalone server.
# The server only needs QtCore, QtNetwork and QtXml; it does not link the LMMS core.

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
)
set_target_properties(lmms-collab-server PROPERTIES AUTOMOC ON)
target_link_libraries(lmms-collab-server PRIVATE
	collabprotocol
	Qt${QT_VERSION_MAJOR}::Core
	Qt${QT_VERSION_MAJOR}::Network
	Qt${QT_VERSION_MAJOR}::Xml
)
install(TARGETS lmms-collab-server RUNTIME DESTINATION "${BIN_DIR}")
