# target_compile_features has been introduced in that version
cmake_minimum_required (VERSION 3.1...3.27)
project (sdn VERSION 1.0 LANGUAGES CXX)

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU")
	set (CMAKE_CXX_FLAGS
		"${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-misleading-indentation -pedantic")
endif ()

find_package (PkgConfig REQUIRED)
pkg_check_modules (ACL libacl)
pkg_check_modules (NCURSESW ncursesw)
if (NOT NCURSESW_FOUND)
	find_library (NCURSESW_LIBRARIES NAMES ncursesw)
	find_path (NCURSESW_INCLUDE_DIRS ncurses.h PATH_SUFFIXES ncurses)
endif ()

add_executable (${PROJECT_NAME} ${PROJECT_NAME}.cpp)
target_include_directories (${PROJECT_NAME}
	PUBLIC ${NCURSESW_INCLUDE_DIRS} ${ACL_INCLUDE_DIRS})
target_link_directories (${PROJECT_NAME}
	PUBLIC ${NCURSESW_LIBRARY_DIRS} ${ACL_LIBRARY_DIRS})
target_link_libraries (${PROJECT_NAME}
	PUBLIC ${NCURSESW_LIBRARIES} ${ACL_LIBRARIES})
target_compile_features (${PROJECT_NAME} PUBLIC cxx_std_14)
target_compile_definitions (${PROJECT_NAME} PUBLIC
	-DPROJECT_NAME=\"${PROJECT_NAME}\" -DPROJECT_VERSION=\"${PROJECT_VERSION}\")

add_executable (${PROJECT_NAME}-mc-ext ${PROJECT_NAME}-mc-ext.cpp)
target_compile_features (${PROJECT_NAME}-mc-ext PUBLIC cxx_std_17)

include (GNUInstallDirs)
# sdn-mc-ext should be in libexec, but we prefer it in PATH.
install (TARGETS sdn sdn-mc-ext
	DESTINATION ${CMAKE_INSTALL_BINDIR})
install (PROGRAMS sdn-install sdn-view
	DESTINATION ${CMAKE_INSTALL_BINDIR})
install (FILES sdn.1 sdn-install.1 sdn-view.1
	DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install (FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Directory navigator")
set (CPACK_PACKAGE_VENDOR "Premysl Eric Janouch")
set (CPACK_PACKAGE_CONTACT "Přemysl Eric Janouch <p@janouch.name>")
set (CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set (CPACK_GENERATOR "TGZ;ZIP")
set (CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set (CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}-${PROJECT_VERSION}")
set (CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set (CPACK_SOURCE_IGNORE_FILES "/\\\\.git;/build;/CMakeLists.txt.user")
set (CPACK_SOURCE_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}")

set (CPACK_SET_DESTDIR TRUE)
include (CPack)
