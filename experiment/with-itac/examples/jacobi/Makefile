# Programs for compiling
PROG_CHAM = jacobi_cham

# Chameleon libs
CH_HOME=/dss/dsshome1/0A/di49mew/chameleon_tool_dev/install/with_itac/cham_tool
CH_LIB_TOOL=$(CH_HOME)/lib

# Compiler declarations
MPICXX = /lrz/sys/intel/impi2019u6/impi/2019.6.154/intel64/lrzbin/mpicxx
MPIRUN = /lrz/sys/intel/impi2019u6/impi/2019.6.154/intel64/bin/mpirun
CXX = /lrz/sys/intel/studio2019_u5/compilers_and_libraries_2019.5.281/linux/bin/intel64/icpc
CXX_FLAGS = -Wall
CXX_OMP_FLAGS = -qopenmp
LINKER_FLAGS = -lm -qopenmp -lstdc++ -lchameleon -L${CH_LIB_TOOL}

$(PROG_CHAM): jacobi_cham.cpp
	$(MPICXX) $(CXX_FLAGS) -o $(PROG_CHAM) jacobi_cham.cpp $(LINKER_FLAGS)

clean:
	rm -f *.o $(PROG_CHAM)