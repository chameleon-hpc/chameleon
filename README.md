# CHAMELEON
A Task-based Programming Environment for Developing Reactive HPC Applications

## 1. Description

The technical design of High Performance Computing (HPC) systems is becoming increasingly complex. The Chameleon project is dedicated to the aspect of ever-increasing dynamic variability. The programming approaches widely used today are often poorly suited for highly dynamic systems and may in the future only be able to exploit part of the true potential of modern systems.

Project Chameleon developed a task-based programming environment based on the established standards MPI and OpenMP that is better prepared for systems with dynamic variability than today's widely used bulk-synchronous programming models.

In Chameleon, the dynamic performance data required for efficient task execution is provided by a monitoring or introspection component. Without negatively affecting the target application or introducing high overhead, performance metrics are determined and made available to the application itself, or to Chameleon's execution system. For example, the computational load or power consumption of the computational system components are continuously monitored and made available to the other parts of Chameleon. With such a frequent and reactive monitoring it is then possible to form better load balancing decisions not just in shared memory but also between MPI processes in distributed memory.
The concept in Chameleon is based on migratable task entities that can either be executed on the local system or depending on the current load situation be migrated to a remote rank and executed there.

### Funding

CHAMELEON is funded by the German Ministry of Education and Research (BMBF) (2017-2020).

### Further information

**Website:**

http://www.chameleon-hpc.org/

**GitHub:**

https://github.com/chameleon-hpc/chameleon

## 2. Publications / How to Cite

* Jannis Klinkenberg, Philipp Samfass, Michael Bader, Christian Terboven, Matthias S. Müller. **CHAMELEON: Reactive Load Balancing for Hybrid MPI+OpenMP Task-Parallel Applications**, Journal of Parallel and Distributed Computing, Volume 138, 2020, Pages 55-64, ISSN 0743-7315. https://doi.org/10.1016/j.jpdc.2019.12.005

* Jannis Klinkenberg, Philipp Samfass, Michael Bader, Christian Terboven, Matthias S. Müller. (2020) **Reactive Task Migration for Hybrid MPI+OpenMP Applications**. In: Wyrzykowski R., Deelman E., Dongarra J., Karczewski K. (eds) Parallel Processing and Applied Mathematics. PPAM 2019. Lecture Notes in Computer Science, vol 12044. Springer, Cham. https://doi.org/10.1007/978-3-030-43222-5_6

* Philipp Samfass, Jannis Klinkenberg, and Michael Bader. **Hybrid MPI+OpenMP Reactive Work Stealing in Distributed Memory in the PDE Framework sam(oa)^2**. Proceedings of the 2018 IEEE International Conference on Cluster Computing (CLUSTER). September 10 - 13, 2018, Belfast, UK. https://ieeexplore.ieee.org/document/8514894

## 3. Building and Installing CHAMELEON

CHAMELEON can be build from source using CMake.

### 3.1. Prerequisites

- CMake version 2.8.12 or greater
- C++ compiler supporting the C++11 standard and OpenMP
- Fortran compiler (optional)
- MPI installation (OpenMPI, MPICH, ...)
- hwloc

### 3.2. Getting the source 

    $ git clone https://github.com/chameleon-hpc/chameleon.git

### 3.3. Building

To build CHAMELEON using CMake run:

Create build directory:

    $ mkdir BUILD && cd ./BUILD

Run cmake to configure the build:

    $ cmake ..

For a list of available parameters:

    $ cmake .. -L

To configure build parameters using ccmake:

    $ ccmake ..

To build run:

    $ make

### 3.4. Installation

Installing to the default installation path usually requires root privileges. Changing the installation path can be done using `-DCMAKE_INSTALL_PREFIX`

    $ cmake -DCMAKE_INSTALL_PREFIX=<install/path> ../
    $ make
    $ make install
