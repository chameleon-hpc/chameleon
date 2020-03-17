# Some declarations
FILE_NAMES ?= mxm_unequal_tasks.cpp
PROG ?=mxm_unequal_tasks
TOOL ?= tool
NUM_TASKS_R0 ?= 100	# for make run
NUM_TASKS_R1 ?= 100 # for make run
NUM_RANKS ?= 2

# Compiler
MPICXX = /lrz/sys/intel/impi2019u6/impi/2019.6.154/intel64/lrzbin/mpicxx
MPIRUN = /lrz/sys/intel/impi2019u6/impi/2019.6.154/intel64/bin/mpirun
CXX = /lrz/sys/intel/studio2019_u5/compilers_and_libraries_2019.5.281/linux/bin/intel64/icpc

# Chameleon libs
CH_HOME=/dss/dsshome1/0A/di49mew/chameleon_tool_dev/install/with_itac/cham_tool
CH_LIB_TOOL=$(CH_HOME)/lib


# Parameters for running
OMPI_PARAMETERS = --bind-to-core 

# Flags for optimization
APP_COMPILE_FLAGS= -std=c++11
LINKER_FLAGS_CHAM_TOOL= -fopenmp -lm -lstdc++ -lchameleon -L${CH_LIB_TOOL}

FLAGS_OPTIMIZATION=-g -O3

$(PROG)_tool: $(PROG).cpp
	${MPICXX} $(FLAGS_OPTIMIZATION) -o $(PROG)_tool $(APP_COMPILE_FLAGS) $(FILE_NAMES) $(LINKER_FLAGS_CHAM_TOOL)

$(TOOL): $(TOOL).cpp $(TOOL).h
	$(CXX) -g -O0 -shared -std=c++11 -fPIC -o $(TOOL).so $(TOOL).cpp -L$(CH_HOME)/lib -I$(CH_HOME)include

clean:
	rm -f $(PROG)_* $(TOOL).so *.o
