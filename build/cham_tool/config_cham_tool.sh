# remove some previous compiled versions
rm -rf ./CMakeCache.txt  ./CMakeFiles  ./cmake_install.cmake ./Makefile  ./src
#rm -rf ../insta_lrz_stand/*

# Load hwloc 2.0.1
module load hwloc/2.0.1

# Load itac
module load itac

# load module cmake on LRZ to use the C++ std11
module load cmake

# Export libffi
# SET PKG CONFIG with FFI
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/dss/dsshome1/0A/di49mew/loc-libs/libffi-3.3/build/lib/pkgconfig
export LD_LIBRARY_PATH=/dss/dsshome1/0A/di49mew/loc-libs/libffi-3.3/build/lib64:$LD_LIBRARY_PATH
export LIBRARY_PATH=/dss/dsshome1/0A/di49mew/loc-libs/libffi-3.3/build/lib64:$LIBRARY_PATH
export INCLUDE=/dss/dsshome1/0A/di49mew/loc-libs/libffi-3.3/build/include:$INCLUDE
export CPATH=/dss/dsshome1/0A/di49mew/loc-libs/libffi-3.3/build/include:/dss/dsshome1/lrz/sys/spack/release/19.1/opt/x86_avx512/hwloc/2.0.1-gcc-gir2kom/include:$CPATH

export OMP_PLACES=cores
export OMP_PROC_BIND=close
export I_MPI_PIN=1
export I_MPI_PIN_DOMAIN=auto
export I_MPI_FABRICS="shm:tmi"
export I_MPI_DEBUG=5
export KMP_AFFINITY=verbose

# Remember to set the link.txt in the folder chameleon.dir: add the path of libffi after -libff
# Add more 2 line in the file /CMakeModules/FFI.cmake in the chameleon source code
#set(FFI_INCLUDE_DIR "/dss/dsshome1/0A/di49mew/loc-libs/libffi-3.3/build/include")
#set(FFI_LIBRARY_DIR "/dss/dsshome1/0A/di49mew/loc-libs/libffi-3.3/build/lib64")

# Run cmake
cmake -DCMAKE_INSTALL_PREFIX=/dss/dsshome1/0A/di49mew/chameleon_tool/install/with_itac/cham_tool    \
    -DCMAKE_C_COMPILER=/lrz/sys/intel/studio2019_u5/compilers_and_libraries_2019.5.281/linux/bin/intel64/icc    \
    -DCMAKE_CXX_COMPILER=/lrz/sys/intel/studio2019_u5/compilers_and_libraries_2019.5.281/linux/bin/intel64/icpc \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS='-DOFFLOAD_SEND_TASKS_SEPARATELY=1' \
    -DCMAKE_C_FLAGS='-DOFFLOAD_SEND_TASKS_SEPARATELY=1' \
    /dss/dsshome1/0A/di49mew/chameleon_tool
