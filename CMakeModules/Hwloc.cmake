find_path(
    HWLOC_PREFIX
    NAMES include/hwloc.h
)

if (NOT HWLOC_PREFIX AND NOT $ENV{HWLOC_BASE} STREQUAL "")
    set(HWLOC_PREFIX $ENV{HWLOC_BASE})
endif()

find_library(
    HWLOC_LIBRARIES
    NAMES hwloc
    HINTS ${HWLOC_PREFIX}/lib
)

find_path(
    HWLOC_INCLUDE_DIRS
    NAMES hwloc.h
    HINTS ${HWLOC_PREFIX}/include
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    HWLOC DEFAULT_MSG
    HWLOC_LIBRARIES
    HWLOC_INCLUDE_DIRS
)

if (HWLOC_FOUND)
    message(STATUS "HWLOC Includes:  " ${HWLOC_INCLUDE_DIRS})
    message(STATUS "HWLOC Libraries: " ${HWLOC_LIBRARIES})
endif()