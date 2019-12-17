CHAMELEON
====

A Task-based Programming Environment for Developing Reactive HPC Applications

Description
-------

tbd.

Funding
-------

CHAMELEON is funded by the German Ministry of Education and Research (BMBF) (2017-2020).


Further information
---------

**Website:**

http://www.chameleon-hpc.org/

**GitHub:**

https://github.com/chameleon-hpc/chameleon


Building and Installing CHEMELEON
============

CHAMELEON can be build from source using CMake.

### 0. Prerequisites

- CMake version 2.8.12 or greater
- C++ compiler supporting the C++11 standard and OpenMP
- Fortran compiler (optional)
- MPI installation (OpenMPI, MPICH, ...)
- hwloc

### 1. Getting the source 

    $ git clone https://github.com/chameleon-hpc/chameleon.git

### 2. Building

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

### 3. Installation

Installing to the default installation path usually requires root privileges. Changing the installation path can be done using `-DCMAKE_INSTALL_PREFIX`

    $ cmake -DCMAKE_INSTALL_PREFIX=<install/path> ../
    $ make
    $ make install
