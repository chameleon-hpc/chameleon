#INSTALL_DIR=~/chameleon/
INSTALL_DIR ?=~/install/chameleon-lib/intel_1.0

FILES_CXX_COMP=chameleon.cpp commthread.cpp chameleon_statistics.cpp chameleon_strategies.cpp chameleon_common.cpp request_manager.cpp chameleon_tools.cpp
FILES_F90_COMP=chameleon_lib.f90
FILES_COPY_INCLUDE=chameleon.h chameleon_tools.h 
FILES_COPY_INCLUDE_FORTRAN=chameleon_lib.mod
FILES_COPY_LIB=libchameleon.so

# default is release
FLAGS_OPTIMIZATION=-g -O3

ifneq ($(TARGET),)
include ../flags_$(TARGET).def
else
include ../flags_claix_intel.def
endif

# custom compile flags that can be added via e.g. commandline or scripts
CUSTOM_COMPILE_FLAGS ?=
CH_CXX_COMPILE_FLAGS += $(CUSTOM_COMPILE_FLAGS)
CH_F90_COMPILE_FLAGS += $(CUSTOM_COMPILE_FLAGS)

default: release

dbg: FLAGS_OPTIMIZATION=-g -O0
dbg: generic

debug: FLAGS_OPTIMIZATION=-g -O0 -DCHAM_DEBUG
debug: generic

release: FLAGS_OPTIMIZATION=-g -O3
release: generic

trace: FLAGS_OPTIMIZATION=-g -O3 -DTRACE -I$(VT_ROOT)/include -trace
trace: generic

generic:
	@if [ ! -z "$(MPIF90)" -a "$(MPIF90)" != "" ]; then\
		echo "$(MPIF90) $(FLAGS_OPTIMIZATION) $(CH_F90_COMPILE_FLAGS) -c $(FILES_F90_COMP)"; \
		$(MPIF90) $(FLAGS_OPTIMIZATION) $(CH_F90_COMPILE_FLAGS) -c $(FILES_F90_COMP); \
	fi
	$(MPICXX) $(FLAGS_OPTIMIZATION) $(CH_CXX_COMPILE_FLAGS) -c $(FILES_CXX_COMP) 	
	$(MPICXX) $(FLAGS_OPTIMIZATION) $(CH_CXX_COMPILE_FLAGS) -o libchameleon.so *.o $(CH_LINKER_FLAGS)

	cp $(FILES_COPY_INCLUDE) $(INSTALL_DIR)/include/
	cp $(FILES_COPY_INCLUDE_FORTRAN) $(INSTALL_DIR)/include/ &> /dev/null | true
	cp $(FILES_COPY_LIB) $(INSTALL_DIR)/lib/

clean:
	rm -f *.so *.o *.mod
