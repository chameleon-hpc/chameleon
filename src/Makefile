INSTALL_DIR=~/chameleon
#INSTALL_DIR=~/install/chameleon-lib/1.0
#INSTALL_DIR=~/install/chameleon-lib/1.0-zero-copy

CXX_COMP_FILES=chameleon.cpp commthread.cpp cham_statistics.cpp cham_strategies.cpp chameleon_common.cpp request_manager.cpp chameleon_tools.cpp
LINKER_FLAGS=-lffi -lpthread -lhwloc -ldl
CXX_COMPILE_FLAGS=-std=c++11 -fpic -shared

F_COMP_FILES=chameleon_lib.f90
F90_COMPILE_FLAGS=-fpic

FILES_COPY_INCLUDE=chameleon.h chameleon_tools.h 
FILE_COPY_INCLUDE_FORTRAN=chameleon_lib.mod
FILES_COPY_LIB=libchameleon.so

# ===== Clang compiler
#MPICXX=I_MPI_CXX=clang++ mpiicpc
# === used for target offloading
# FLAGS_OPENMP=-fopenmp -fopenmp-targets=x86_64-unknown-linux-gnu
# === used with maunal API call
#FLAGS_OPENMP=-fopenmp

# ===== Intel compiler
MPICXX=I_MPI_CXX=icpc mpiicpc
MPIF90=I_MPI_F90=ifort mpiifort
FLAGS_OPENMP=-qopenmp -qno-openmp-offload -Wno-unknown-pragmas
#FLAGS_OPENMP=-qopenmp -qopenmp-offload=host -Wno-unknown-pragmas

default: release

dbg:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -g -O0 $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -o libchameleon.so $(CXX_COMP_FILES) $(LINKER_FLAGS)
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

debug:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -g -O0 $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -DCHAM_DEBUG -c $(CXX_COMP_FILES) 
	$(MPIF90) -g -O0 $(FLAGS_OPENMP) $(F90_COMPILE_FLAGS) -DCHAM_DEBUG -c chameleon_lib.f90 
	$(MPICXX) $(CXX_COMPILE_FLAGS) -shared *.o -o libchameleon.so $(LINKER_FLAGS)
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

release:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	@#$(MPICXX) -g -O3 $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -o libchameleon.so $(CXX_COMP_FILES) $(LINKER_FLAGS)
	$(MPICXX) -g -O3 $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -c $(CXX_COMP_FILES) 
	$(MPICXX) $(CXX_COMPILE_FLAGS) -shared *.o -o libchameleon.so $(LINKER_FLAGS)	

	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

release_fortran:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	@#$(MPICXX) -g -O3 $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -o libchameleon.so $(CXX_COMP_FILES) $(LINKER_FLAGS)
	$(MPICXX) -g -O3 $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -c $(CXX_COMP_FILES) 
	$(MPIF90) -g -O3 $(FLAGS_OPENMP) $(F90_COMPILE_FLAGS) -c chameleon_lib.f90 
	$(MPICXX) $(CXX_COMPILE_FLAGS) -shared *.o -o libchameleon.so $(LINKER_FLAGS)	

	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_INCLUDE_FORTRAN) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/
trace:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -O3 -g -DTRACE -I$(VT_ROOT)/include $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -c $(CXX_COMP_FILES)
	$(MPIF90) -g -O3 -DTRACE -I$(VT_ROOT)/include $(FLAGS_OPENMP) $(F90_COMPILE_FLAGS) -c chameleon_lib.f90 
	$(MPICXX) $(CXX_COMPILE_FLAGS) -shared *.o -o libchameleon.so $(LINKER_FLAGS) -trace	
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

trace-debug:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -O0 -g -DCHAM_DEBUG -DTRACE -I$(VT_ROOT)/include $(FLAGS_OPENMP) $(CXX_COMPILE_FLAGS) -o libchameleon.so $(CXX_COMP_FILES) $(LINKER_FLAGS) -trace
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

clean:
	rm -f *.so *.o *.mod
