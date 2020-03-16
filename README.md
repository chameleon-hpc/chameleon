CHAMELEON_TOOL
====

A tool for profiling chamaleon-behaviors

Description
-------

Building and Linking the tool
============

CHAMELEON can be build from source using CMake.

### 1. Changes when we integrate the tool
There are 4 main files that I have defined some data structures & callback functions:
- chameleon_tools.h: define a seperate struct for the tool
```cpp
/*****************************************************************************
 * Data for the tool
 ****************************************************************************/
typedef struct cham_t_task_info_t {
    TYPE_TASK_ID task_id;
    int rank_belong;    // 0
    size_t size_data;   // 1
```

- chameleon_tools.cpp: add one more callback function for changing the frequency
```cpp
// I linked to the tool by hand like here
cham_t_start_tool_result_t * cham_t_start_tool(unsigned int cham_version) {
    cham_t_start_tool_result_t *ret = NULL;
    // Search next symbol in the current address space. This can happen if the
    // runtime library is linked before the tool. Since glibc 2.2 strong symbols
    // don't override weak symbols that have been found before unless the user
    // sets the environment variable LD_DYNAMIC_WEAK.
    void *handle = dlopen("/dss/dsshome1/0A/di49mew/chameleon_tool_dev/experiment/with-itac/cham_tool/tool.so", RTLD_LAZY);
    if (!handle){
        fprintf(stderr, "%s\n", dlerror());
        exit(EXIT_FAILURE);
    }
```
```cpp
		case cham_t_callback_change_freq_for_execution:
            cham_t_status.cham_t_callback_change_freq_for_execution = (cham_t_callback_change_freq_for_execution_t)callback;
            break;
```

- chameleon_tools_internal.h: add the enum for this callback function
```cpp
typedef struct cham_t_callbacks_active_s {
    unsigned int enabled : 1;
    
    // list of callback pointers
    // ...
    cham_t_callback_change_freq_for_execution_t     cham_t_callback_change_freq_for_execution   = nullptr;  // change frequency

} cham_t_callbacks_active_t;
```

- chameleon.cpp: turn on the flag to catch the event of this callback
```cpp
int32_t execute_target_task(cham_migratable_task_t *task) {
    // ... ...

    // Add some noise here when executing the task
#if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_change_freq_for_execution && chameleon_comm_rank != 0) {
        int32_t noise_time = cham_t_status.cham_t_callback_change_freq_for_execution(task);
        // make the process slower by sleep
        DBP("execute_target_task - noise_time = %d\n", noise_time);
        usleep(noise_time);
    }
#endif
	// ... ...
```

To build CHAMELEON using CMake run: build as usual

### 2. Run & compile the tool
As the example in /chameleon_tool_dev/experiment/with-itac, there are 2 seperate commands for compiling the tool and example.
```python
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
```

Here we have the tool outside Chameleon's core. We can change or add more callback functions here (tool.cpp and tool.h).