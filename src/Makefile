#INSTALL_DIR=~/chameleon
INSTALL_DIR=~/install/chameleon-lib/1.0
#INSTALL_DIR=~/install/chameleon-lib/1.0-zero-copy

COMP_FILES=chameleon.cpp commthread.cpp cham_statistics.cpp cham_strategies.cpp chameleon_common.cpp request_manager.cpp
MPICXX=I_MPI_CXX=clang++ mpiicpc
FLAGS_OPENMP=-fopenmp -fopenmp-targets=x86_64-unknown-linux-gnu
#MPICXX=I_MPI_CXX=icpc mpiicpc
#FLAGS_OPENMP=-qopenmp -qno-openmp-offload -Wno-unknown-pragmas
LINKER_FLAGS=-lffi -lpthread -lhwloc -ldl

default: release

dbg:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -g -O0 $(FLAGS_OPENMP)  -std=c++11 -fpic -shared -o libchameleon.so $(COMP_FILES) $(LINKER_FLAGS)
	
	cp chameleon.h $(INSTALL_DIR)/include/
	cp libchameleon.so $(INSTALL_DIR)/lib/

debug:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -g -O0 $(FLAGS_OPENMP)  -std=c++11 -fpic -shared -DCHAM_DEBUG -o libchameleon.so $(COMP_FILES) $(LINKER_FLAGS)
	
	cp chameleon.h $(INSTALL_DIR)/include/
	cp libchameleon.so $(INSTALL_DIR)/lib/

release:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -g -O3 $(FLAGS_OPENMP) -std=c++11 -fpic -shared -o libchameleon.so $(COMP_FILES) $(LINKER_FLAGS)
	
	cp chameleon.h $(INSTALL_DIR)/include/
	cp libchameleon.so $(INSTALL_DIR)/lib/

trace:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPI_CXX) -O3 -g -DTRACE -I$(VT_ROOT)/include $(FLAGS_OPENMP) -std=c++11 -fpic -shared -o libchameleon.so $(COMP_FILES) $(LINKER_FLAGS) -trace
	
	cp chameleon.h $(INSTALL_DIR)/include/
	cp libchameleon.so $(INSTALL_DIR)/lib/

trace-debug:
	@# compile with target offloading to force image loading process, all target images should be loaded at program start
	$(MPICXX) -O0 -g -DCHAM_DEBUG -DTRACE -I$(VT_ROOT)/include $(FLAGS_OPENMP) -std=c++11 -fpic -shared -o libchameleon.so $(COMP_FILES) $(LINKER_FLAGS) -trace
	
	cp chameleon.h $(INSTALL_DIR)/include/
	cp libchameleon.so $(INSTALL_DIR)/lib/

clean:
	rm -f *.so *.o
