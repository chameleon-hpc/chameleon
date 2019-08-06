cmake_minimum_required(VERSION 2.8.12)
enable_language(Fortran)
project(chameleon_main)

# set c++11 standard as base
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

## Additional inclued necessary to build chameleon
include(${CMAKE_SOURCE_DIR}/CMakeModules/MPI.cmake)
include(${CMAKE_SOURCE_DIR}/CMakeModules/OpenMP.cmake)
include(${CMAKE_SOURCE_DIR}/CMakeModules/Hwloc.cmake)
include(${CMAKE_SOURCE_DIR}/CMakeModules/FFI.cmake)
## Set compiler flags depending on compiler
include(${CMAKE_SOURCE_DIR}/CMakeModules/CompileFlags.cmake)

## Version settings
set(CHAMELEON_VERSION_MAJOR 0 CACHE STRING "CHAMELEON major version")
set(CHAMELEON_VERSION_MINOR 1 CACHE STRING "CHAMELEON minor version")
set(CHAMELEON_VERSION_BUILD 0 CACHE STRING "CHAMELEON build version")
mark_as_advanced(CHAMELEON_VERSION_MAJOR CHAMELEON_VERSION_MINOR CHAMELEON_VERSION_BUILD)
set(CHAMELEON_VERSION "${CHAMELEON_VERSION_MAJOR}.${CHAMELEON_VERSION_MINOR}.${CHAMELEON_VERSION_BUILD}" CACHE STRING INTERNAL FORCE)

# Default build type
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# add sub directories
add_subdirectory(src)