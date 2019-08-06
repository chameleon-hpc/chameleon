# compiler specific additions
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    # using Clang
    if(ENABLE_TARGET_OFFLOADING)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fopenmp-targets=x86_64-unknown-linux-gnu" CACHE STRING "" FORCE)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp-targets=x86_64-unknown-linux-gnu" CACHE STRING "" FORCE)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fopenmp-targets=x86_64-unknown-linux-gnu" CACHE STRING "" FORCE)
    endif()
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    # using GCC
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Intel")
    # using Intel
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -qno-openmp-offload -Wno-unknown-pragmas" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -qno-openmp-offload -Wno-unknown-pragmas" CACHE STRING "" FORCE)
    set(CMAKE_Fortran_FLAGS "${CMAKE_Fortran_FLAGS} -qno-openmp-offload" CACHE STRING "" FORCE)
endif()