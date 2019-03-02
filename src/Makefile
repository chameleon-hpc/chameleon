#INSTALL_DIR=~/chameleon
INSTALL_DIR=~/install/chameleon-lib/1.0

CXX_COMP_FILES=chameleon.cpp commthread.cpp cham_statistics.cpp cham_strategies.cpp chameleon_common.cpp request_manager.cpp chameleon_tools.cpp
CXX_COMPILE_FLAGS=-std=c++11 -fpic

F90_COMP_FILES=chameleon_lib.f90
F90_COMPILE_FLAGS=-fpic

FILES_COPY_INCLUDE=chameleon.h chameleon_tools.h 
FILES_COPY_INCLUDE_FORTRAN=chameleon_lib.mod
FILES_COPY_LIB=libchameleon.so

# ===== Clang compiler
MPICXX=I_MPI_CXX=clang++ mpiicpc
# === used for target offloading
FLAGS_OPENMP=-fopenmp -fopenmp-targets=x86_64-unknown-linux-gnu
LINKER_FLAGS=-shared -lffi -lpthread -lhwloc -ldl
# === used with maunal API call
#FLAGS_OPENMP=-fopenmp

# ===== Intel compiler
#MPICXX=I_MPI_CXX=icpc mpiicpc
#MPIF90=I_MPI_F90=ifort mpiifort
#FLAGS_OPENMP=-qopenmp -qno-openmp-offload -Wno-unknown-pragmas
#FLAGS_OPENMP=-qopenmp -qopenmp-offload=host -Wno-unknown-pragmas
#LINKER_FLAGS=-shared -lffi -lpthread -lhwloc -ldl -lifcore

default: release

dbg:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPIF90) -g -O0 $(FLAGS_OPENMP) $(F90_COMPILE_FLAGS) -c $(F90_COMP_FILES) 
	$(MPICXX) -g -O0 $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -c $(CXX_COMP_FILES) 	
	$(MPICXX) $(CXX_COMPILE_FLAGS) -o libchameleon.so *.o $(LINKER_FLAGS)

	@# Somehow only this version is working for the target offloading version
	@#$(MPICXX) -g -O0 $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -o libchameleon.so $(CXX_COMP_FILES) $(LINKER_FLAGS)
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_INCLUDE_FORTRAN) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

debug:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPIF90) -g -O0 $(FLAGS_OPENMP) $(F90_COMPILE_FLAGS) -DCHAM_DEBUG -c $(F90_COMP_FILES) 
	$(MPICXX) -g -O0 $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -DCHAM_DEBUG -c $(CXX_COMP_FILES) 
	$(MPICXX) $(CXX_COMPILE_FLAGS) -o libchameleon.so *.o $(LINKER_FLAGS)

	@# Somehow only this is working for the target offloading version
	@#$(MPICXX) -g -O0 $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -DCHAM_DEBUG -o libchameleon.so $(CXX_COMP_FILES) $(LINKER_FLAGS)
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_INCLUDE_FORTRAN) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

release:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPIF90) -O3 -g $(FLAGS_OPENMP) $(F90_COMPILE_FLAGS) -c $(F90_COMP_FILES)
	$(MPICXX) -O3 -g $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -c $(CXX_COMP_FILES)
	$(MPICXX) $(CXX_COMPILE_FLAGS) -o libchameleon.so *.o $(LINKER_FLAGS)

	@# Somehow only this is working for the target offloading version
	@#$(MPICXX) -O3 -g $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -o libchameleon.so $(CXX_COMP_FILES) $(LINKER_FLAGS)

	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_INCLUDE_FORTRAN) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

trace:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPIF90) -O3 -g -DTRACE -I$(VT_ROOT)/include $(FLAGS_OPENMP) $(F90_COMPILE_FLAGS) -c $(F90_COMP_FILES) 
	$(MPICXX) -O3 -g -DTRACE -I$(VT_ROOT)/include $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -c $(CXX_COMP_FILES)
	$(MPICXX) $(CXX_COMPILE_FLAGS) -o libchameleon.so *.o $(LINKER_FLAGS) -trace

	@# Somehow only this is working for the target offloading version
	@#$(MPICXX) -O3 -g $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -o libchameleon.so $(CXX_COMP_FILES) $(LINKER_FLAGS) -trace
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_INCLUDE_FORTRAN) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

clean:
	rm -f *.so *.o *.mod
