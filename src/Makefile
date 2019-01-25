#INSTALL_DIR=~/chameleon
INSTALL_DIR=~/install/chameleon-lib/1.0
#INSTALL_DIR=~/install/chameleon-lib/1.0-zero-copy

COMP_FILES=chameleon.cpp commthread.cpp cham_statistics.cpp cham_strategies.cpp chameleon_common.cpp request_manager.cpp chameleon_tools.cpp
LINKER_FLAGS=-lffi -lpthread -lhwloc -ldl
COMPILE_FLAGS=-std=c++11 -fpic -shared

FILES_COPY_INCLUDE=chameleon.h chameleon_tools.h
FILES_COPY_LIB=libchameleon.so

# ===== Clang compiler
#MPICXX=I_MPI_CXX=clang++ mpiicpc
# === used for target offloading
# FLAGS_OPENMP=-fopenmp -fopenmp-targets=x86_64-unknown-linux-gnu
# === used with maunal API call
#FLAGS_OPENMP=-fopenmp

# ===== Intel compiler
MPICXX=I_MPI_CXX=icpc mpiicpc
FLAGS_OPENMP=-qopenmp -qno-openmp-offload -Wno-unknown-pragmas
#FLAGS_OPENMP=-qopenmp -qopenmp-offload=host -Wno-unknown-pragmas

default: release

dbg:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -g -O0 $(FLAGS_OPENMP) $(COMPILE_FLAGS) -o libchameleon.so $(COMP_FILES) $(LINKER_FLAGS)
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

debug:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -g -O0 $(FLAGS_OPENMP) $(COMPILE_FLAGS) -DCHAM_DEBUG -o libchameleon.so $(COMP_FILES) $(LINKER_FLAGS)
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

release:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -g -O3 $(FLAGS_OPENMP) $(COMPILE_FLAGS) -o libchameleon.so $(COMP_FILES) $(LINKER_FLAGS)
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

trace:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPI_CXX) -O3 -g -DTRACE -I$(VT_ROOT)/include $(FLAGS_OPENMP) $(COMPILE_FLAGS) -o libchameleon.so $(COMP_FILES) $(LINKER_FLAGS) -trace
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

trace-debug:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -O0 -g -DCHAM_DEBUG -DTRACE -I$(VT_ROOT)/include $(FLAGS_OPENMP) $(COMPILE_FLAGS) -o libchameleon.so $(COMP_FILES) $(LINKER_FLAGS) -trace
	
	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

clean:
	rm -f *.so *.o
