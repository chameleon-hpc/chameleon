INSTALL_DIR=~/chameleon/test
#INSTALL_DIR ?=~/install/chameleon-lib/intel_1.0

CXX_COMP_FILES=chameleon.cpp commthread.cpp cham_statistics.cpp cham_strategies.cpp chameleon_common.cpp request_manager.cpp chameleon_tools.cpp
CXX_COMPILE_FLAGS=-std=c++11 -fpic -DCHAM_STATS_RECORD -DCHAM_STATS_PRINT
F90_COMP_FILES=chameleon_lib.f90
F90_COMPILE_FLAGS=-fpic

FILES_COPY_INCLUDE=chameleon.h chameleon_tools.h 
FILES_COPY_INCLUDE_FORTRAN=chameleon_lib.mod
FILES_COPY_LIB=libchameleon.so

ifeq ($(CXX),clang++)
    MPICXX=I_MPI_CXX=clang++ mpiicpc
    FLAGS_OPENMP=-fopenmp -fopenmp-targets=x86_64-unknown-linux-gnu # used with target offloading
    #FLAGS_OPENMP=-fopenmp # used with manual API
    LINKER_FLAGS=-shared -lffi -lpthread -lhwloc -ldl -fopenmp-targets=x86_64-unknown-linux-gnu
else ifeq ($(CXX),icpc)
    MPICXX=I_MPI_CXX=icpc mpiicpc -mt_mpi
    MPIF90=I_MPI_F90=ifort mpiifort -mt_mpi
    FLAGS_OPENMP=-qopenmp -qno-openmp-offload -Wno-unknown-pragmas
    LINKER_FLAGS=-shared -lffi -lpthread -lhwloc -ldl -lifcore
endif

CXX_COMPILE_FLAGS +=$(FLAGS_OPENMP)
F90_COMPILE_FLAGS +=$(FLAGS_OPENMP)

default: release

dbg:
	@if [ "$(CXX)" = "icpc" ]; then\
		$(MPIF90) -g -O0 $(F90_COMPILE_FLAGS) -c $(F90_COMP_FILES); \
	fi
	$(MPICXX) -g -O0 $(CXX_COMPILE_FLAGS) -c $(CXX_COMP_FILES) 	
	$(MPICXX) $(CXX_COMPILE_FLAGS) -o libchameleon.so *.o $(LINKER_FLAGS)

	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	@if [ "$(CXX)" = "icpc" ]; then\
		cp $(FILES_COPY_INCLUDE_FORTRAN) $(INSTALL_DIR)/include/; \
	fi
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

debug:
	@if [ "$(CXX)" = "icpc" ]; then\
		$(MPIF90) -g -O0 $(F90_COMPILE_FLAGS) -DCHAM_DEBUG -c $(F90_COMP_FILES); \
	fi
	$(MPICXX) -g -O0 $(CXX_COMPILE_FLAGS) -DCHAM_DEBUG -c $(CXX_COMP_FILES) 
	$(MPICXX) $(CXX_COMPILE_FLAGS) -o libchameleon.so *.o $(LINKER_FLAGS)

	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	@if [ "$(CXX)" = "icpc" ]; then\
		cp $(FILES_COPY_INCLUDE_FORTRAN) $(INSTALL_DIR)/include/; \
	fi
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

release:
	@if [ "$(CXX)" = "icpc" ]; then\
		$(MPIF90) -g -O3 $(F90_COMPILE_FLAGS) -c $(F90_COMP_FILES); \
	fi	
	$(MPICXX) -g -O3 $(CXX_COMPILE_FLAGS) -c $(CXX_COMP_FILES)
	$(MPICXX) -g $(CXX_COMPILE_FLAGS) -o libchameleon.so *.o $(LINKER_FLAGS)

	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	@if [ "$(CXX)" = "icpc" ]; then\
		cp $(FILES_COPY_INCLUDE_FORTRAN) $(INSTALL_DIR)/include/; \
	fi
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

trace:
	@if [ "$(CXX)" = "icpc" ]; then\
		$(MPIF90) -g -O3 -DTRACE -I$(VT_ROOT)/include $(F90_COMPILE_FLAGS) -c $(F90_COMP_FILES); \
	fi
	$(MPICXX) -g -O3 -DTRACE -I$(VT_ROOT)/include $(CXX_COMPILE_FLAGS) -c $(CXX_COMP_FILES)
	$(MPICXX) $(CXX_COMPILE_FLAGS) -o libchameleon.so *.o $(LINKER_FLAGS) -trace

	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	@if [ "$(CXX)" = "icpc" ]; then\
		cp $(FILES_COPY_INCLUDE_FORTRAN) $(INSTALL_DIR)/include/; \
	fi
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

clean:
	rm -f *.so *.o *.mod
