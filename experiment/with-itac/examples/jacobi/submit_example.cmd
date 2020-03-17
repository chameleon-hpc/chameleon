#!/bin/sh
#SBATCH -J cham_jacobi
#SBATCH -o ./results/ch-jacobi_%J.out
#SBATCH -e ./results/ch-jacobi_%J.err
#SBATCH -D ./
#SBATCH --time=00:05:00
#SBATCH --get-user-env
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --account=pr58ci
#SBATCH --partition=test

module load slurm_setup

export OMP_NUM_THREADS=1
export IS_DISTRIBUTED=1
export IS_SEPARATE=0
export N_PROCS=2
export OMP_PLACES=cores
export OMP_PROC_BIND=close
export VT_LOGFILE_PREFIX=/dss/dsshome1/0A/di49mew/chameleon_tool_dev/experiment/with-itac/examples/jacobi/results/itac_traces

# Run the program with cham_tool
mpirun -n 2 /dss/dsshome1/0A/di49mew/chameleon_tool_dev/experiment/with-itac/examples/jacobi/jacobi_cham 2 2
 
##mpirun -trace -n 2 /dss/dsshome1/0A/di49mew/chameleon_tool_dev/experiment/with-itac/cham_tool/jacobi_cham 1 10
