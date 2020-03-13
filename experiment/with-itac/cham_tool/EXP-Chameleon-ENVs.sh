# load hwloc on SuperMUC-NG
module load hwloc

# CHAM_MODE
export CHAM_MODE=cham_tool

# export libffi & hwloc
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/dss/dsshome1/0A/di49mew/loc-libs/libffi-3.3/build/lib/pkgconfig
export LD_LIBRARY_PATH=/dss/dsshome1/0A/di49mew/loc-libs/libffi-3.3/build/lib64:$LD_LIBRARY_PATH
export CPATH=/dss/dsshome1/0A/di49mew/loc-libs/libffi-3.3/build/include:/dss/dsshome1/lrz/sys/spack/release/19.1/opt/x86_avx512/hwloc/2.0.1-gcc-gir2kom/include:$CPATH

# export ch-libs
export LD_LIBRARY_PATH=/dss/dsshome1/0A/di49mew/chameleon_tool/install/with_itac/$CHAM_MODE/lib:$LD_LIBRARY_PATH
export LIBRARY_PATH=/dss/dsshome1/0A/di49mew/chameleon_tool/install/with_itac/$CHAM_MODE/lib:$LIBRARY_PATH
export INCLUDE=/dss/dsshome1/0A/di49mew/chameleon_tool/install/with_itac/$CHAM_MODE/include:$INCLUDE
export CPATH=/dss/dsshome1/0A/di49mew/chameleon_tool/install/with_itac/$CHAM_MODE/include:$CPATH
