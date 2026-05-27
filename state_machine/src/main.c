#include <zephyr/kernel.h>
#include <zephyr/smf.h>
#include <stdio.h>

/* Thread Configurations */
#define GPS_STACK_SIZE 2048
#define GPS_THREAD_PRIORITY 3  

/* 1. Define State Machine States */
enum tracker_state {
    STATE_INIT,
    STATE_WAIT_GPS_FIX,
    STATE_CHECK_FOB,
    STATE_CONNECT_NETWORK,
    STATE_SEND_BATCH
};

/* 2. State Machine Context Struct */
struct tracker_machine {
    struct smf_ctx ctx;             
    struct k_mutex data_lock;       
    
    /* Telemetry Counters */
    int fake_gps_buffer;            
    uint32_t next_point_id;         
    uint32_t batch_start_id;        
    
    /* State Control Flags */
    bool network_ready;
    bool gps_fixed;
    int gps_fix_countdown;          
    bool fob_present;
    bool is_first_fob_check;
};

static struct tracker_machine my_tracker;
static const struct smf_state tracker_states[];

/* ===================================================================
 * 3. State Actions
 * =================================================================== */

// --- STATE: INIT ---
static void s_init_entry(void *obj)
{
    struct tracker_machine *machine = (struct tracker_machine *)obj;
    printf("[SMF] Initializing tracking engine and mutex systems...\n");
    
    k_mutex_init(&machine->data_lock);

    k_mutex_lock(&machine->data_lock, K_FOREVER);
    machine->network_ready = false;
    machine->gps_fixed = false;
    machine->gps_fix_countdown = 3; 
    machine->fake_gps_buffer = 0;
    machine->next_point_id = 1;
    machine->batch_start_id = 1;
    machine->is_first_fob_check = true;

    machine->fob_present = (k_cycle_get_32() % 2 == 0);
    printf("[BOOT] BLE Fob 50%% Startup Check: %s\n", machine->fob_present ? "FOUND" : "NOT FOUND");
    k_mutex_unlock(&machine->data_lock);
}

static enum smf_state_result s_init_run(void *obj)
{
    struct tracker_machine *machine = (struct tracker_machine *)obj;
    smf_set_state(SMF_CTX(machine), &tracker_states[STATE_WAIT_GPS_FIX]);
    return SMF_EVENT_HANDLED;
}

// --- STATE: WAIT GPS FIX ---
static enum smf_state_result s_wait_gps_fix_run(void *obj)
{
    struct tracker_machine *machine = (struct tracker_machine *)obj;
    
    k_mutex_lock(&machine->data_lock, K_FOREVER);
    if (machine->gps_fix_countdown > 0) {
        printf("[SMF] Waiting for GPS fix... (%ds remaining)\n", machine->gps_fix_countdown);
        machine->gps_fix_countdown--;
    } else {
        printf("[SMF] >>> SUCCESS: GPS Lock Acquired!\n");
        machine->gps_fixed = true;
        smf_set_state(SMF_CTX(machine), &tracker_states[STATE_CHECK_FOB]);
    }
    k_mutex_unlock(&machine->data_lock);
    
    return SMF_EVENT_HANDLED;
}

// --- STATE: CHECK FOB (Modified to check for Buffer size of 3) ---
static enum smf_state_result s_check_fob_run(void *obj)
{
    struct tracker_machine *machine = (struct tracker_machine *)obj;
    printf("[SMF] Scanning BLE spectrum for keyfob token...\n");
    
    k_mutex_lock(&machine->data_lock, K_FOREVER);
    if (machine->is_first_fob_check) {
        machine->is_first_fob_check = false; 
    } else {
        machine->fob_present = false; // Simulate owner missing after boot
    }

    if (machine->fob_present) {
        printf(" -> Fob Present: Privacy mode active (Data cleared).\n");
        machine->fake_gps_buffer = 0; 
        machine->batch_start_id = machine->next_point_id; 
        smf_set_state(SMF_CTX(machine), &tracker_states[STATE_CHECK_FOB]);
    } else {
        /* CRITICAL RULE: Check if buffer payload threshold is met */
        if (machine->fake_gps_buffer >= 8) {
            printf(" -> Fob Absent & Buffer threshold reached (%d/8)! Triggering transmission state.\n", machine->fake_gps_buffer);
            smf_set_state(SMF_CTX(machine), &tracker_states[STATE_CONNECT_NETWORK]);
        } else {
            printf(" -> Fob Absent but buffer is too low (%d/3). Remaining in scanning cycle.\n", machine->fake_gps_buffer);
            smf_set_state(SMF_CTX(machine), &tracker_states[STATE_CHECK_FOB]);
        }
    }
    k_mutex_unlock(&machine->data_lock);
    
    return SMF_EVENT_HANDLED;
}

// --- STATE: CONNECT NETWORK ---
static enum smf_state_result s_connect_run(void *obj)
{
    struct tracker_machine *machine = (struct tracker_machine *)obj;
    printf("[SMF] Connecting to cellular network tower...\n");
    
    k_mutex_lock(&machine->data_lock, K_FOREVER);
    machine->network_ready = true; 
    smf_set_state(SMF_CTX(machine), &tracker_states[STATE_SEND_BATCH]);
    k_mutex_unlock(&machine->data_lock);
    
    return SMF_EVENT_HANDLED;
}

// --- STATE: SEND BATCH ---
static enum smf_state_result s_send_run(void *obj)
{
    struct tracker_machine *machine = (struct tracker_machine *)obj;
    printf("[SMF] Finalizing MQTT payload serialization...\n");
    
    k_mutex_lock(&machine->data_lock, K_FOREVER);
    if (machine->fake_gps_buffer > 0) {
        uint32_t batch_end_id = machine->next_point_id - 1;
        
        printf("[MOCK_MQTT] >>> SUCCESS: Transmitted packet envelope containing %d points.\n", machine->fake_gps_buffer);
        printf("[MOCK_MQTT] >>> SENT RANGE: IDs [#%u to #%u]\n", machine->batch_start_id, batch_end_id);
        
        machine->fake_gps_buffer = 0;
        machine->batch_start_id = machine->next_point_id; 
    } else {
        printf("[MOCK_MQTT] >>> ABORT: Data buffer empty.\n");
    }
    
    smf_set_state(SMF_CTX(machine), &tracker_states[STATE_CHECK_FOB]);
    k_mutex_unlock(&machine->data_lock);
    
    return SMF_EVENT_HANDLED;
}

/* ===================================================================
 * 4. Master State Table Setup
 * =================================================================== */
static const struct smf_state tracker_states[] = {
    [STATE_INIT]            = SMF_CREATE_STATE(s_init_entry, s_init_run, NULL, NULL, NULL),
    [STATE_WAIT_GPS_FIX]    = SMF_CREATE_STATE(NULL,         s_wait_gps_fix_run, NULL, NULL, NULL),
    [STATE_CHECK_FOB]       = SMF_CREATE_STATE(NULL,         s_check_fob_run, NULL, NULL, NULL),
    [STATE_CONNECT_NETWORK] = SMF_CREATE_STATE(NULL,         s_connect_run, NULL, NULL, NULL),
    [STATE_SEND_BATCH]      = SMF_CREATE_STATE(NULL,         s_send_run, NULL, NULL, NULL),
};

/* ===================================================================
 * 5. Background Threads (Thread-Safe GPS Accumulator)
 * =================================================================== */
void gps_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    while (1) {
        k_msleep(1000); 
        
        k_mutex_lock(&my_tracker.data_lock, K_FOREVER);
        if (my_tracker.gps_fixed) {
            my_tracker.fake_gps_buffer++;
            printf("   [GPS_THREAD] Logged Point ID #%u (Current Buffer Size: %d)\n", 
                   my_tracker.next_point_id, my_tracker.fake_gps_buffer);
            my_tracker.next_point_id++; 
        } else {
            printf("   [GPS_THREAD] No fix. Satellite tracking offline.\n");
        }
        k_mutex_unlock(&my_tracker.data_lock);
    }
}

K_THREAD_DEFINE(gps_thread_id, GPS_STACK_SIZE, gps_thread_entry, NULL, NULL, NULL, GPS_THREAD_PRIORITY, 0, 0);

/* ===================================================================
 * 6. Main Thread Application Entry Point
 * =================================================================== */
int main(void)
{
    printf("\n--- Zephyr OS Multi-Threaded Tracker Kernel Booting ---\n");

    smf_set_initial(SMF_CTX(&my_tracker), &tracker_states[STATE_INIT]);

    while (1) {
        k_msleep(1000);
        
        int ret = smf_run_state(SMF_CTX(&my_tracker));
        if (ret != 0) {
            printf("[CRITICAL] State machine threw runtime anomaly: %d\n", ret);
            break;
        }
    }
    return 0;
}