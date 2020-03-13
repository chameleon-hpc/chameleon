#!/bin/sh
#SBATCH -J cham_tool_test
#SBATCH -o ./results/ch-mxm_tool_10x10_%J.out
#SBATCH -e ./results/ch-mxm_tool_10x10_%J.err
#SBATCH -D ./
#SBATCH --time=00:05:00
#SBATCH --get-user-env
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --account=pr58ci
#SBATCH --partition=test

module load slurm_setup

export OMP_NUM_THREADS=2
export IS_DISTRIBUTED=1
export IS_SEPARATE=0
export N_PROCS=2
export OMP_PLACES=cores
export OMP_PROC_BIND=close
export VT_LOGFILE_PREFIX=/dss/dsshome1/0A/di49mew/chameleon_tool_dev/experiment/with-itac/cham_tool/results/itac_traces

# Run the program with cham_tool
CHAMELEON_TOOL=1 CHAMELEON_TOOL_LIBRARIES=1 mpirun -trace -n 2 /dss/dsshome1/0A/di49mew/chameleon_tool_dev/experiment/with-itac/cham_tool/mxm_unequal_tasks_tool 1 10
 
##mpirun -trace -n 2 /dss/dsshome1/0A/di49mew/chameleon_tool_dev/experiment/with-itac/cham_tool/mxm_unequal_tasks_tool 1 10
