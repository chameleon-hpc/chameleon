#ifndef COMPILE_CHAMELEON
#define COMPILE_CHAMELEON 1
#endif

#ifndef PARALLEL_INIT
#define PARALLEL_INIT 1
#endif


#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <mpi.h>
#include "chameleon.h"

static int N = 10;
static int MAX_ITERATIONS = 20000;
static int SEED = 0;
static double CONVERGENCE_THRESHOLD = 0.01;

/* Define functions */
#define SPEC_RESTRICT __restrict__
#define LOG(rank, str) printf("#R%d: %s\n", rank, str)

double get_timestamp()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec*1e-6;
}

int parse_int(const char *str)
{
  char *next;
  int value = strtoul(str, &next, 10);
  return strlen(next) ? -1 : value;
}

double parse_double(const char *str)
{
  char *next;
  double value = strtod(str, &next);
  return strlen(next) ? -1 : value;
}

void parse_arguments(int argc, char *argv[]){
    // Set default values
    N = 10000;
    MAX_ITERATIONS = 20000;
    CONVERGENCE_THRESHOLD = 0.0001;
    SEED = 0;

    for (int i = 1; i < argc; i++){
        // check arg: --convergence
        if (!strcmp(argv[i], "--convergence") || !strcmp(argv[i], "-c")){
            if (++i >= argc || (CONVERGENCE_THRESHOLD = parse_int(argv[i])) < 0){
            printf("Invalid convergence threshold\n");
            exit(1);
            }
        // check arg: --iterations
        }else if (!strcmp(argv[i], "--iterations") || !strcmp(argv[i], "-i")){
            if (++i >= argc || (MAX_ITERATIONS = parse_int(argv[i])) < 0){
            printf("Invalid number of iterations\n");
            exit(1);
            }
        }else if (!strcmp(argv[i], "--norder") || !strcmp(argv[i], "-n")){
            if (++i >= argc || (N = parse_int(argv[i])) < 0){
                printf("Invalid matrix order\n");
                exit(1);
            }
        }
        else if (!strcmp(argv[i], "--seed") || !strcmp(argv[i], "-s")){
            if (++i >= argc || (SEED = parse_int(argv[i])) < 0){
                printf("Invalid seed\n");
                exit(1);
            }
        }else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")){
            printf("\n");
            printf("Usage: ./jacobi [OPTIONS]\n\n");
            printf("Options:\n");
            printf("  -h  --help               Print this message\n");
            printf("  -c  --convergence  C     Set convergence threshold\n");
            printf("  -i  --iterations   I     Set maximum number of iterations\n");
            printf("  -n  --norder       N     Set maxtrix order\n");
            printf("  -s  --seed         S     Set random number seed\n");
            printf("\n");
            exit(0);
        }else{
            printf("Unrecognized argument '%s' (try '--help')\n", argv[i]);
            exit(1);
        }
    }
}

void print_matrix(double *A, int N){
    for(int i = 0; i < N; i++){
        for(int j = 0; j < N; j++){
            printf("%.2f ", A[i + j*N]);
        }
        printf("\n");
    }
    printf("\n");
}

void print_vector(double *v, int N){
    for(int i = 0; i < N; i++){
        printf("%.3f ", v[i]);
    }
    printf("\n");
}

int solver(double * SPEC_RESTRICT A, double * SPEC_RESTRICT b, double * SPEC_RESTRICT x, double * SPEC_RESTRICT xtmp){
    int itr;
    int row, col;
    double dot;
    double diff;
    double sqdiff;
    double *ptrtmp;

    /* For example: 
    A00x0   +   A01x1   +   A02x2   =   b0
    A10x0   +   A11x1   +   A12x2   =   b1
    A20x0   +   A21x1   +   A22x2   =   b2
    --------------------------------------
    x0_0 = 0, x1_0 = 0, x2_0 = 0
    --------------------------------------
    x0_1 = [b0 - (A01x1_0 + A02x2_0)] / A00
    x1_1 = [b1 - (A10x0_0 + A12x2_0)] / A11
    ...
    --------------------------------------*/

    itr = 0;
    do{
        for (row = 0; row < N; row++){
            dot = 0.0;
            for (col = 0; col < N; col++){
                if (row != col)
                    // caculate the dot
                    dot += A[row + col * N]  * x[col];
            }
            // update x from x0
            xtmp[row] = (b[row] - dot) / A[row + row * N];
        }

        // swap pointers to update new values for x
        ptrtmp = x;
        x = xtmp;
        xtmp = ptrtmp;

        // check the convergence
        sqdiff = 0.0;
        for (row = 0; row < N; row++){
            diff = xtmp[row] - x[row];
            sqdiff += diff * diff;
        }

        // increase the iteration
        itr++;
    } while ((itr < MAX_ITERATIONS) && (sqrt(sqdiff) > CONVERGENCE_THRESHOLD));

    return itr;
}

void init_data(double *A, double *b, double *x, int mat_size)
{
    srand(SEED);
    int N = mat_size;
    for (int row = 0; row < N; row++){
        double rowsum = 0.0;
        for (int col = 0; col < N; col++){
            double value = rand()/(double)RAND_MAX;
            A[row+ col*N] = value;
            rowsum += value;
        }
        A[row + row*N] += rowsum;
        b[row] = rand()/(double)RAND_MAX;
        x[row] = 0.0;
    }
}

void jacobi_kernel(double * SPEC_RESTRICT cham_A, double * SPEC_RESTRICT cham_b, double * SPEC_RESTRICT cham_x, double * SPEC_RESTRICT cham_xtmp, int size, int i){
    int num_itr = solver(cham_A, cham_b, cham_x, cham_xtmp);
}

double check_result(double *A, double *b, double *x, int size){
    double err = 0.0;
    int N = size;
    for (int row = 0; row < N; row++){
        double tmp = 0.0;
        for (int col = 0; col < N; col++){
            tmp += A[row + col*N] * x[col];
        }
        tmp = b[row] - tmp;
        err += tmp*tmp;
    }
    err = sqrt(err);
    return err;
}

int main(int argc, char *argv[]){

    // vars and initial
    int iMyRank, iNumProcs;
    int provided;
	int requested = MPI_THREAD_MULTIPLE;
	MPI_Init_thread(&argc, &argv, requested, &provided);
	MPI_Comm_size(MPI_COMM_WORLD, &iNumProcs);
	MPI_Comm_rank(MPI_COMM_WORLD, &iMyRank);
    int numberOfTasks;  // num of jacobi tasks
	double fTimeStart, fTimeEnd;    // for time measurement
	double wTimeCham, wTimeHost;

    // init chameleon
#if COMPILE_CHAMELEON
    #pragma omp parallel
    {
        chameleon_thread_init();
    }
    // necessary to be aware of binary base addresses to
    // calculate offset for target entry functions
    chameleon_determine_base_addresses((void *)&main);
#endif /* COMPILE_CHAMELEON */

    // get arguments
    // parse_arguments(argc, argv);
    if (argc == 3){
        numberOfTasks = atoi(argv[iMyRank + 1]);
    }else{
        printf("Usage: mpirun -n 2 ./jacobi_cham <num_task_R0> <num_task_R1>\n");
        return 0;
    }

    printf("-------------------Rank-%d------------------------\n", iMyRank);
    printf("Matrix size:            %dx%d\n", N, N);
    printf("Maximum iterations:     %d\n", MAX_ITERATIONS);
    printf("Convergence threshold:  %lf\n", CONVERGENCE_THRESHOLD);
    printf("-------------------------------------------\n");

    double **A    = new double*[numberOfTasks];
    double **b    = new double*[numberOfTasks];
    double **x    = new double*[numberOfTasks];
    double **xtmp = new double*[numberOfTasks];

    // allocate and initialize matrices
#if PARALLEL_INIT
    if(iMyRank == 0) {
        printf("Executing parallel init\n");
    }
    #pragma omp parallel for
#endif
    for (int i = 0; i < numberOfTasks; i++) {
 		A[i] = new double[N*N];
    	b[i] = new double[N];
    	x[i] = new double[N];
        xtmp[i] = new double[N];
        // init data
        init_data(A[i], b[i], x[i], N);
    }
    // printf("Matrix A:\n");
    // print_matrix(A, N);
    // printf("-------------------------------------------\n");
    // printf("vector b:\n");
    // print_vector(b, N);
    // printf("-------------------------------------------\n");
    // printf("vector x:\n");
    // print_vector(x, N);
    // printf("-------------------------------------------\n");
    MPI_Barrier(MPI_COMM_WORLD);

    // create tasks with chameleon
#if COMPILE_CHAMELEON
    fTimeStart = MPI_Wtime();
    #pragma omp parallel
    {
        #pragma omp for
    	for(int i = 0; i < numberOfTasks; i++) {
            double * SPEC_RESTRICT cham_A = A[i];
            double * SPEC_RESTRICT cham_b = b[i];
            double * SPEC_RESTRICT cham_x = x[i];
            double * SPEC_RESTRICT cham_xtmp = xtmp[i];
            void* literal_mat_size   = *(void**)(&N);
            void* literal_i          = *(void**)(&i);

            // create data with chameleon
            chameleon_map_data_entry_t *args = new chameleon_map_data_entry_t[6];
            args[0] = chameleon_map_data_entry_create(cham_A, N*N*sizeof(double), CHAM_OMP_TGT_MAPTYPE_TO);
            args[1] = chameleon_map_data_entry_create(cham_b, N*sizeof(double), CHAM_OMP_TGT_MAPTYPE_TO);
            args[2] = chameleon_map_data_entry_create(cham_x, N*sizeof(double), CHAM_OMP_TGT_MAPTYPE_FROM);
            args[3] = chameleon_map_data_entry_create(cham_xtmp, N*sizeof(double), CHAM_OMP_TGT_MAPTYPE_FROM);
            args[4] = chameleon_map_data_entry_create(literal_mat_size, sizeof(void*), CHAM_OMP_TGT_MAPTYPE_TO | CHAM_OMP_TGT_MAPTYPE_LITERAL);
            args[5] = chameleon_map_data_entry_create(literal_i, sizeof(void*), CHAM_OMP_TGT_MAPTYPE_TO | CHAM_OMP_TGT_MAPTYPE_LITERAL);
            // create task
            printf("Task %d belongs R%d, mat_size = %d\n", i, iMyRank, N);
            cham_migratable_task_t *task = chameleon_create_task((void *)&jacobi_kernel, 6, args);
            // add task to the queue
            int32_t res = chameleon_add_task(task);
            // delete parameter arr
            delete[] args;
        }

        // distribute tasks and execute
	    // printf("[Debug] Call chameleon_distributed_taskwait()\n");
    	int res = chameleon_distributed_taskwait(0);

        // wait for ...
        #pragma omp single
        MPI_Barrier(MPI_COMM_WORLD);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    // get execution_time for each process
    fTimeEnd = MPI_Wtime();
    wTimeCham = fTimeEnd - fTimeStart;

    // check error
    double err = 0.0;
    if(numberOfTasks > 0) {
        bool pass =  true;
        for(int t = 0; t < numberOfTasks; t++) {
            err = check_result(A[t], b[t], x[t], N);
            if (err > CONVERGENCE_THRESHOLD)
                pass = false;
        }
        if(pass)
            LOG(iMyRank, "TEST SUCCESS");
        else
            LOG(iMyRank, "TEST FAILED");
    }

    //deallocate matrices
    for(int i = 0; i < numberOfTasks; i++) {
    	delete[] A[i];
    	delete[] b[i];
    	delete[] x[i];
        delete[] xtmp[i];
    }
    delete[] A;
    delete[] b;
    delete[] x; 
    delete[] xtmp;
#endif // COMPILE_CHAMELEON
    MPI_Barrier(MPI_COMM_WORLD);

    printf("Solution error = %lf\n", err);
    printf("Total runtime  = %lf seconds\n", wTimeCham);
    // printf("Solver runtime = %lf seconds\n", (solve_end-solve_start));
    // if (itr == MAX_ITERATIONS)
    //     printf("WARNING: solution did not converge\n");
    // printf("-------------------------------------------\n");

#if COMPILE_CHAMELEON
    #pragma omp parallel
    {
        chameleon_thread_finalize();
    }
    chameleon_finalize();
#endif
    MPI_Finalize();
    return 0;
}