#include "tool.h"

#define TASK_TOOL_SAMPLE_DATA_SIZE 10

static cham_t_set_callback_t cham_t_set_callback;
static cham_t_get_callback_t cham_t_get_callback;
static cham_t_get_rank_data_t cham_t_get_rank_data;
static cham_t_get_thread_data_t cham_t_get_thread_data;
static cham_t_get_rank_info_t cham_t_get_rank_info;
static cham_t_get_task_data_t cham_t_get_task_data;
// static cham_t_get_unique_id_t cham_t_get_unique_id;

//================================================================
// Variables
//================================================================

//================================================================
// Additional functions
//================================================================

int compare( const void *pa, const void *pb ){
    const int *a = (int *) pa;
    const int *b = (int *) pb;
    printf("a[0] = %d, b[0] = %d\n", a[0], b[0]);
    if(a[0] == b[0])
        return a[0] - b[0];
    else
        return a[1] - b[1];
}

//================================================================
// Callback Functions
//================================================================ 
static void
on_cham_t_callback_thread_init(
    cham_t_data_t *thread_data)
{
    int rank_info       = cham_t_get_rank_info()->comm_rank;
    thread_data->value = syscall(SYS_gettid);
}

static void
on_cham_t_callback_thread_finalize(
    cham_t_data_t *thread_data)
{
    int rank_info       = cham_t_get_rank_info()->comm_rank;
    thread_data->value = syscall(SYS_gettid);
}

static void
on_cham_t_callback_task_create(
    cham_migratable_task_t * task,
    cham_t_data_t *task_data,
    const void *codeptr_ra)
{
    TYPE_TASK_ID internal_task_id        = chameleon_get_task_id(task);
    double q_time; TIMESTAMP(q_time);
    int rank_info       = cham_t_get_rank_info()->comm_rank;
    // create custom data structure and use task_data as pointer
    cham_t_task_info_t * cur_task  = (cham_t_task_info_t*) malloc(sizeof(cham_t_task_info_t));
    cur_task->task_id           = internal_task_id;
    cur_task->rank_belong       = rank_info;
    cur_task->queue_time        = q_time;
    tool_task_list.push_back(cur_task);
}

static void
on_cham_t_callback_task_schedule(
    cham_migratable_task_t * task,                   // opaque data type for internal task
    cham_t_task_flag_t task_flag,
    cham_t_data_t *task_data,
    cham_t_task_schedule_type_t schedule_type,
    cham_migratable_task_t * prior_task,             // opaque data type for internal task
    cham_t_task_flag_t prior_task_flag,
    cham_t_data_t *prior_task_data)
{
    TYPE_TASK_ID task_id = chameleon_get_task_id(task);
    double disp_time; TIMESTAMP(disp_time);
    int rank = cham_t_get_rank_info()->comm_rank;
    tool_task_list.set_start_time(task_id, disp_time);
}

static int32_t
on_cham_t_callback_determine_local_load(
    TYPE_TASK_ID* task_ids_local,
    int32_t num_tasks_local,
    TYPE_TASK_ID* task_ids_local_rep,
    int32_t num_tasks_local_rep,
    TYPE_TASK_ID* task_ids_stolen,
    int32_t num_tasks_stolen,
    TYPE_TASK_ID* task_ids_stolen_rep,
    int32_t num_tasks_stolen_rep)
{
    double time_stmp; TIMESTAMP(time_stmp);
    int rank_info       = cham_t_get_rank_info()->comm_rank;
    return num_tasks_local;
}

static cham_t_migration_tupel_t*
on_cham_t_callback_select_tasks_for_migration(
    const int32_t* load_info_per_rank,
    TYPE_TASK_ID* task_ids_local,
    int32_t num_tasks_local,
    int32_t num_tasks_stolen,
    int32_t* num_tuples)
{
    cham_t_rank_info_t *rank_info  = cham_t_get_rank_info();
    cham_t_migration_tupel_t* task_migration_tuples = NULL;
    *num_tuples = 0;

    if (num_tasks_local > 0){
        task_migration_tuples = (cham_t_migration_tupel_t *)malloc(sizeof(cham_t_migration_tupel_t));   // allocate mem for the tuple
        // sort loads by rank
        int tmp_sorted_array[rank_info->comm_size][2];
        int i;
        for (i = 0; i < rank_info->comm_size; i++){
            tmp_sorted_array[i][0] = i; // contain rank index
            tmp_sorted_array[i][1] = load_info_per_rank[i]; // contain load per rank
        }
        // check the values
        // for(i = 0; i < rank_info->comm_size; ++i)
        //     printf("Rank-%d, load=%d\n", tmp_sorted_array[i][0], tmp_sorted_array[i][1]);

        qsort(tmp_sorted_array, rank_info->comm_size, sizeof tmp_sorted_array[0], compare);

        // check after sorting
        // for(i = 0; i < rank_info->comm_size; ++i)
        //     printf("Rank-%d, load=%d\n", tmp_sorted_array[i][0], tmp_sorted_array[i][1]);

        int min_val = load_info_per_rank[tmp_sorted_array[0][0]];   // load rank 0
        int max_val = load_info_per_rank[tmp_sorted_array[rank_info->comm_size-1][0]];  // load rank 1
        int load_this_rank = load_info_per_rank[rank_info->comm_rank];
        // printf("min is R%d = %d, max is R%d = %d\n", tmp_sorted_array[0][0], min_val, tmp_sorted_array[rank_info->comm_size-1][0], max_val);

        if (max_val > min_val){
            int pos = 0;
            for (i = 0; i < rank_info->comm_size; i++){
                if (tmp_sorted_array[i][0] == rank_info->comm_rank){
                    pos = i;
                    break;
                }
            }

            // only offload if on the upper side
            if((pos+1) >= ((double)rank_info->comm_size/2.0))
            {
                int other_pos = rank_info->comm_size-pos;
                // need to adapt in case of even number
                if(rank_info->comm_size % 2 == 0)
                    other_pos--;
                int other_idx = tmp_sorted_array[other_pos][0];
                int other_val = load_info_per_rank[other_idx];
                // calculate ration between those two and just move if over a certain threshold
                double ratio = (double)(load_this_rank-other_val) / (double)load_this_rank;
                if(other_val < load_this_rank && ratio > 0.5) {
                    double mig_time; TIMESTAMP(mig_time);
                    task_migration_tuples[0].task_id = task_ids_local[0];
                    task_migration_tuples[0].rank_id = other_idx;
                    tool_task_list.set_migrated_time(task_ids_local[0], mig_time);
                    *num_tuples = 1;
                }
            }
        }

        if(*num_tuples <= 0)
        {
            free(task_migration_tuples);
            task_migration_tuples = NULL;
        }
    }

    return task_migration_tuples;
}

static int32_t
on_cham_t_callback_determine_local_load(
    cham_migratable_task_t * task
)
{
    int32_t noise_time = 100000;
    return noise_time;
}

//================================================================
// Start Tool & Register Callbacks
//================================================================

#define register_callback_t(name, type)                                         \
do{                                                                             \
    type f_##name = &on_##name;                                                 \
    if (cham_t_set_callback(name, (cham_t_callback_t)f_##name) == cham_t_set_never)   \
        printf("0: Could not register callback '" #name "'\n");                 \
} while(0)

#define register_callback(name) register_callback_t(name, name##_t)

int cham_t_initialize(
    cham_t_function_lookup_t lookup,
    cham_t_data_t *tool_data)
{
    printf("Calling register_callback...\n");
    cham_t_set_callback = (cham_t_set_callback_t) lookup("cham_t_set_callback");
    // cham_t_get_callback = (cham_t_get_callback_t) lookup("cham_t_get_callback");
    cham_t_get_rank_data = (cham_t_get_rank_data_t) lookup("cham_t_get_rank_data");
    cham_t_get_thread_data = (cham_t_get_thread_data_t) lookup("cham_t_get_thread_data");
    cham_t_get_rank_info = (cham_t_get_rank_info_t) lookup("cham_t_get_rank_info");
    // cham_t_get_task_data = (cham_t_get_task_data_t) lookup("cham_t_get_task_data");

    register_callback(cham_t_callback_thread_init);
    register_callback(cham_t_callback_thread_finalize);
    register_callback(cham_t_callback_task_create);
    register_callback(cham_t_callback_task_schedule);
    // register_callback(cham_t_callback_encode_task_tool_data);
    // register_callback(cham_t_callback_decode_task_tool_data);
    // register_callback(cham_t_callback_sync_region);
    register_callback(cham_t_callback_determine_local_load);
    register_callback(cham_t_callback_select_tasks_for_migration);
    register_callback(cham_t_callback_change_freq_for_execution);

    // Priority is cham_t_callback_select_tasks_for_migration (fine-grained)
    // if not registered cham_t_callback_select_num_tasks_to_offload is used (coarse-grained)
    // register_callback(cham_t_callback_select_num_tasks_to_offload);
    // register_callback(cham_t_callback_select_num_tasks_to_replicate);

    // cham_t_rank_info_t *r_info  = cham_t_get_rank_info();
    // cham_t_data_t * r_data      = cham_t_get_rank_data();
    // r_data->value               = r_info->comm_rank;

    return 1; //success
}

void cham_t_finalize(cham_t_data_t *tool_data)
{
    printf("------------------------- Chameleon Statistics ---------------------\n");
    int rank_info       = cham_t_get_rank_info()->comm_rank;
    if (rank_info == 0){
        printf("Task ID \t queued_time \t start_time \t mig_time\n");
        for (std::list<cham_t_task_info_t*>::iterator it=tool_task_list.task_list.begin(); it!=tool_task_list.task_list.end(); ++it) {
            printf("%d \t %.3f \t %.3f \t %.3f\n", (*it)->task_id, (*it)->queue_time, (*it)->start_time, (*it)->mig_time);
        }
    }else{
        printf("Task ID \t queued_time \t start_time \t mig_time\n");
        for (std::list<cham_t_task_info_t*>::iterator it=tool_task_list.task_list.begin(); it!=tool_task_list.task_list.end(); ++it) {
            printf("%d \t %.3f \t %.3f \t %.3f\n", (*it)->task_id, (*it)->queue_time, (*it)->start_time, (*it)->mig_time);
        }
    }
}

#ifdef __cplusplus
extern "C" {
#endif
cham_t_start_tool_result_t* cham_t_start_tool(unsigned int cham_version)
{
    printf("Starting tool with Chameleon Version: %d\n", cham_version);

    static cham_t_start_tool_result_t cham_t_start_tool_result = {&cham_t_initialize, &cham_t_finalize, 0};

    return &cham_t_start_tool_result;
}
#ifdef __cplusplus
}
#endif