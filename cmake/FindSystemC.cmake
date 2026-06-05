# FindSystemC.cmake
# Locates the SystemC library
# This module defines:
#  SYSTEMC_FOUND - True if SystemC was found
#  SYSTEMC_INCLUDE_DIRS - The SystemC include directory
#  SYSTEMC_LIBRARIES - The SystemC libraries to link against

# Look for SystemC in SYSTEMC_HOME or vcpkg installation
find_path(SYSTEMC_INCLUDE_DIR 
    NAMES systemc.h systemc
    PATHS
        $ENV{SYSTEMC_HOME}/include
        ${CMAKE_PREFIX_PATH}/include
        ${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
        /usr/local/systemc-2.3.3/include
        /usr/local/include
        /usr/include
    PATH_SUFFIXES systemc
)

if(MSVC)
    set(_systemc_lib_names systemc)
else()
    set(_systemc_lib_names systemc)
endif()

# Look for the library with platform-specific paths
if(WIN32)
    # Windows paths
    find_library(SYSTEMC_LIBRARY
        NAMES ${_systemc_lib_names}
        PATHS
            $ENV{SYSTEMC_HOME}/lib
            ${CMAKE_PREFIX_PATH}/lib
            ${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib
    )
    
    find_library(SYSTEMC_LIBRARY_DEBUG
        NAMES ${_systemc_lib_names}
        PATHS
            $ENV{SYSTEMC_HOME}/lib
            ${CMAKE_PREFIX_PATH}/lib
            ${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib
    )
else()
    # Linux/WSL paths
    find_library(SYSTEMC_LIBRARY
        NAMES ${_systemc_lib_names}
        PATHS
            $ENV{SYSTEMC_HOME}/lib-linux64
            $ENV{SYSTEMC_HOME}/lib
            ${CMAKE_PREFIX_PATH}/lib-linux64
            ${CMAKE_PREFIX_PATH}/lib
            /usr/local/systemc-2.3.3/lib-linux64
            /usr/local/lib
            /usr/lib
    )
    
    find_library(SYSTEMC_LIBRARY_DEBUG
        NAMES ${_systemc_lib_names}
        PATHS
            $ENV{SYSTEMC_HOME}/lib-linux64
            $ENV{SYSTEMC_HOME}/lib
            ${CMAKE_PREFIX_PATH}/lib-linux64
            ${CMAKE_PREFIX_PATH}/lib
            /usr/local/systemc-2.3.3/lib-linux64
            /usr/local/lib
            /usr/lib
    )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SystemC 
    REQUIRED_VARS SYSTEMC_INCLUDE_DIR SYSTEMC_LIBRARY
)

if(SYSTEMC_FOUND)
    set(SYSTEMC_INCLUDE_DIRS ${SYSTEMC_INCLUDE_DIR})
    set(SYSTEMC_LIBRARIES ${SYSTEMC_LIBRARY})
    
    if(NOT TARGET SystemC::systemc)
        add_library(SystemC::systemc INTERFACE IMPORTED)
        set_target_properties(SystemC::systemc PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SYSTEMC_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES "${SYSTEMC_LIBRARIES}"
        )
    endif()
endif()

# Debug SystemC variables
message(STATUS "SYSTEMC_FOUND: ${SYSTEMC_FOUND}")
message(STATUS "SystemC include dirs: ${SYSTEMC_INCLUDE_DIRS}")
message(STATUS "SystemC libraries: ${SYSTEMC_LIBRARIES}")

mark_as_advanced(SYSTEMC_INCLUDE_DIR SYSTEMC_LIBRARY SYSTEMC_LIBRARY_DEBUG)