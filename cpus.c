//  DO-NOT-REMOVE begin-copyright-block
// QFlex consists of several software components that are governed by various
// licensing terms, in addition to software that was developed internally.
// Anyone interested in using QFlex needs to fully understand and abide by the
// licenses governing all the software components.
// 
// ### Software developed externally (not by the QFlex group)
// 
//     * [NS-3] (https://www.gnu.org/copyleft/gpl.html)
//     * [QEMU] (http://wiki.qemu.org/License)
//     * [SimFlex] (http://parsa.epfl.ch/simflex/)
//     * [GNU PTH] (https://www.gnu.org/software/pth/)
// 
// ### Software developed internally (by the QFlex group)
// **QFlex License**
// 
// QFlex
// Copyright (c) 2020, Parallel Systems Architecture Lab, EPFL
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
// 
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright notice,
//       this list of conditions and the following disclaimer in the documentation
//       and/or other materials provided with the distribution.
//     * Neither the name of the Parallel Systems Architecture Laboratory, EPFL,
//       nor the names of its contributors may be used to endorse or promote
//       products derived from this software without specific prior written
//       permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE PARALLEL SYSTEMS ARCHITECTURE LABORATORY,
// EPFL BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//  DO-NOT-REMOVE end-copyright-block
/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* Needed early for CONFIG_BSD etc. */
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/config-file.h"
#include "cpu.h"
#include "monitor/monitor.h"
#include "qapi/qmp/qerror.h"
#include "qemu/error-report.h"
#include "sysemu/sysemu.h"
#include "sysemu/block-backend.h"
#include "exec/gdbstub.h"
#include "sysemu/dma.h"
#include "sysemu/hw_accel.h"
#include "sysemu/kvm.h"
#include "sysemu/hax.h"
#include "qmp-commands.h"
#include "exec/exec-all.h"

#include "qemu/thread.h"
#include "sysemu/cpus.h"
#include "sysemu/qtest.h"
#include "qemu/main-loop.h"
#include "qemu/bitmap.h"
#include "qemu/seqlock.h"
#include "tcg.h"
#include "qapi-event.h"
#include "hw/nmi.h"
#include "sysemu/replay.h"
#include "hw/boards.h"
#include "qflex/qflex.h" // for portable_usleep

#if defined(CONFIG_FLEXUS)
#include "qflex/qflex.h"
#endif /* CONFIG_FLEXUS */

#ifdef CONFIG_FLEXUS
#include "../libqflex/flexus_proxy.h"
#include "../libqflex/api.h"

typedef enum simulation_mode {
    NONE,
    TRACE,
    TIMING,
    LASTMODE,
}simulation_mode;
static const char* simulation_mode_strings[] = {
    "NONE",
    "TRACE",
    "TIMING",
    "LASTMODE"
};
typedef struct flexus_state_t {
    simulation_mode mode;
    uint64_t length;
    const char * simulator;
    const char * config_file; // user_postload
    simulator_obj_t* simulator_obj;
    const char* load_dir;
    const char* debug_mode;

}flexus_state_t;

static flexus_state_t flexus_state;

int flexus_in_timing(void){ return flexus_state.mode == TIMING; }
int flexus_in_trace(void) { return flexus_state.mode == TRACE; }
bool flexus_in_simulation(void){ return flexus_in_timing() | flexus_in_trace(); }
bool hasSimulator(void){ return flexus_state.simulator_obj != NULL; }

static void flexus_setUserPostLoadFile (const char *file_name){
    simulator_config(file_name);
}
const char* flexus_simulation_status(void){
    return simulation_mode_strings[flexus_state.mode];
}

void quitFlexus(void){
    if (flexus_in_simulation()) {
        if(hasSimulator())
            simulator_deinit();
        else {
            fprintf(stderr, "no simulator object found!");
            exit(1);
        }
    }
}
void prepareFlexus(void){
    if (flexus_in_simulation()) {
        if(hasSimulator()){
            if (flexus_state.config_file){
                flexus_setUserPostLoadFile(flexus_state.config_file);
            }
            simulator_prepare();
            if (flexus_state.load_dir) {
                flexus_doLoad(flexus_state.load_dir, NULL);
            }
            if (flexus_state.debug_mode) {
                flexus_setDebug(flexus_state.debug_mode, NULL);
            }
        } else {
            fprintf(stderr, "no simulator object found!");
            exit(1);
        }
    }
}
void initFlexus(void){
    if (flexus_in_simulation()) {
        if( hasSimulator()) {
            QFLEX_API_Interface_Hooks_t* hooks = (QFLEX_API_Interface_Hooks_t*)malloc(sizeof(QFLEX_API_Interface_Hooks_t));
            QFLEX_API_get_Interface_Hooks (hooks);
            simulator_init(hooks);
            free(hooks);
        } else {
            fprintf(stderr, "no simulator object found!");
            exit(1);
        }
    }
}
void startFlexus(void){
    if (flexus_in_timing()) {
        if(hasSimulator()) {
            simulator_start();
        } else {
            fprintf(stderr, "no simulator object found!");
            exit(1);
        }
    }
}
void flexus_qmp(qmp_flexus_cmd_t cmd, const char* args, Error **errp){
    if (flexus_in_simulation()) {
        if(hasSimulator()){
            simulator_qmp(cmd, args);

        } else {
            fprintf(stderr, "no simulator object found!");
            exit(1);
        }
    } else {
        error_setg(errp, "flexus is not running");
    }
}

void flexus_addDebugCfg(const char *filename, Error **errp){
    flexus_qmp(QMP_FLEXUS_ADDDEBUGCFG, filename, errp);
}
void flexus_setBreakCPU(const char * value, Error **errp){
    flexus_qmp(QMP_FLEXUS_SETBREAKCPU, value, errp);
}
void flexus_backupStats(const char *filename, Error **errp){
    flexus_qmp(QMP_FLEXUS_BACKUPSTATS, filename, errp);
}
void flexus_disableCategory(const char *component, Error **errp){
    flexus_qmp(QMP_FLEXUS_DISABLECATEGORY, component, errp);
}
void flexus_disableComponent(const char *component, const char *index, Error **errp){
    char* args = malloc((strlen(component)+strlen(index)*sizeof(char)));
    sprintf(args, "%s:%s",component, index);
    flexus_qmp(QMP_FLEXUS_DISABLECOMPONENT, args, errp);
}
void flexus_enableCategory(const char *component, Error **errp){
    flexus_qmp(QMP_FLEXUS_ENABLECATEGORY, component, errp);
}
void flexus_enableComponent(const char *component, const char *index, Error **errp){
    char* args = malloc((strlen(component)+strlen(index)*sizeof(char)));
    sprintf(args, "%s:%s",component, index);
    flexus_qmp(QMP_FLEXUS_ENABLECOMPONENT, args, errp);
}
void flexus_enterFastMode(Error **errp){
    flexus_qmp(QMP_FLEXUS_ENTERFASTMODE, NULL, errp);
}
void flexus_leaveFastMode(Error **errp){
    flexus_qmp(QMP_FLEXUS_LEAVEFASTMODE, NULL, errp);
}
void flexus_listCategories(Error **errp){
    flexus_qmp(QMP_FLEXUS_LISTCATEGORIES, NULL, errp);
}
void flexus_listComponents(Error **errp){
    flexus_qmp(QMP_FLEXUS_LISTCOMPONENTS, NULL, errp);
}
void flexus_listMeasurements(Error **errp){
    flexus_qmp(QMP_FLEXUS_LISTMEASUREMENTS, NULL, errp);
}
void flexus_log(const char *name, const char *interval, const char *regex, Error **errp){
    char* args = malloc((strlen(name)+strlen(interval)+strlen(regex))*sizeof(char));
    sprintf(args, "%s:%s:%s",name, interval,regex);
    flexus_qmp(QMP_FLEXUS_LOG, args, errp);
}
void flexus_parseConfiguration(const char *filename, Error **errp){
    flexus_qmp(QMP_FLEXUS_PARSECONFIGURATION, filename, errp);
}
void flexus_printConfiguration(Error **errp){
    flexus_qmp(QMP_FLEXUS_PRINTCONFIGURATION, NULL, errp);
}
void flexus_printCycleCount(Error **errp){
    flexus_qmp(QMP_FLEXUS_PRINTCYCLECOUNT, NULL, errp);
}
void flexus_printDebugConfiguration(Error **errp){
    flexus_qmp(QMP_FLEXUS_PRINTDEBUGCONFIGURATION, NULL, errp);
}
void flexus_printMMU(const char* cpu, Error **errp){
    flexus_qmp(QMP_FLEXUS_PRINTMMU, cpu, errp);
}
void flexus_printMeasurement(const char *measurement, Error **errp){
    flexus_qmp(QMP_FLEXUS_PRINTMEASUREMENT, measurement, errp);
}
void flexus_printProfile(Error **errp){
    flexus_qmp(QMP_FLEXUS_PRINTPROFILE, NULL, errp);
}
void flexus_quiesce(Error **errp){
    flexus_qmp(QMP_FLEXUS_QUIESCE, NULL, errp);
}
void flexus_reloadDebugCfg(Error **errp){
    flexus_qmp(QMP_FLEXUS_RELOADDEBUGCFG, NULL, errp);
}
void flexus_resetProfile(Error **errp){
    flexus_qmp(QMP_FLEXUS_RESETPROFILE, NULL, errp);
}
void flexus_saveStats(const char *filename, Error **errp){
    flexus_qmp(QMP_FLEXUS_SAVESTATS, filename, errp);
}
void flexus_setBreakInsn(const char *value, Error **errp){
    flexus_qmp(QMP_FLEXUS_SETBREAKCPU, value, errp);
}
void flexus_setConfiguration(const char *name, const char *value, Error **errp){
    char* args = malloc((strlen(name)+strlen(value))*sizeof(char));
    sprintf(args, "%s:%s",name, value);
    flexus_qmp(QMP_FLEXUS_SETCONFIGURATION, args, errp);
}
void flexus_setDebug(const char *debugseverity, Error **errp){
    flexus_qmp(QMP_FLEXUS_SETDEBUG, debugseverity, errp);
}
void flexus_setProfileInterval(const char *value, Error **errp){
    flexus_qmp(QMP_FLEXUS_SETPROFILEINTERVAL, value, errp);
}
void flexus_setRegionInterval(const char *value, Error **errp){
    flexus_qmp(QMP_FLEXUS_SETREGIONINTERVAL, value, errp);
}
void flexus_setStatInterval(const char *value, Error **errp){
    flexus_qmp(QMP_FLEXUS_SETSTATINTERVAL, value, errp);
}
void flexus_setStopCycle(const char *value, Error **errp){
    flexus_qmp(QMP_FLEXUS_SETSTOPCYCLE, value, errp);
}
void flexus_setTimestampInterval(const char *value, Error **errp){
    flexus_qmp(QMP_FLEXUS_SETTIMESTAMPINTERVAL, value, errp);
}
void flexus_terminateSimulation(Error **errp){
    flexus_qmp(QMP_FLEXUS_TERMINATESIMULATION, NULL, errp);
}
void flexus_writeConfiguration(const char *filename, Error **errp){
    flexus_qmp(QMP_FLEXUS_WRITECONFIGURATION, filename, errp);
}
void flexus_writeDebugConfiguration(Error **errp){
    flexus_qmp(QMP_FLEXUS_WRITEDEBUGCONFIGURATION, NULL, errp);
}
void flexus_writeMeasurement(const char *measurement, const char *filename, Error **errp){
    char* args = malloc((strlen(measurement)+strlen(filename))*sizeof(char));
    sprintf(args, "%s:%s",measurement, filename);
    flexus_qmp(QMP_FLEXUS_WRITEMEASUREMENT, args, errp);
}
void flexus_writeProfile(const char *filename, Error **errp){
    flexus_qmp(QMP_FLEXUS_WRITEPROFILE, filename, errp);
}
void flexus_doSave(const char *dir_name, Error **errp){
    flexus_qmp(QMP_FLEXUS_DOSAVE, dir_name, errp);

}
void flexus_doLoad(const char *dir_name, Error **errp){
    int file_count = 0;
    DIR * dirp;
    struct dirent * entry;

    dirp = opendir(dir_name);
    while ((entry = readdir(dirp)) != NULL) {
        if (entry->d_type == DT_REG) { /* If the entry is a regular file */
             file_count++;
        }
    }
    closedir(dirp);

    if (file_count > 2) // might not be best
        flexus_qmp(QMP_FLEXUS_DOLOAD, dir_name, errp);
}

#endif /* CONFIG_FLEXUS */

#ifdef CONFIG_QUANTUM
typedef struct {
    uint64_t quantum_value, quantum_record_value, quantum_node_value,quantum_step_value;
    char* quantum_file_value;
    uint64_t total_num_instructions, last_num_instruction;
    bool quantum_pause;
} quantum_state_t;

static quantum_state_t quantum_state;

bool query_quantum_pause_state(void) { return quantum_state.quantum_pause; }
void quantum_pause(void)    { quantum_state.quantum_pause = true; }
void quantum_unpause(void)  { quantum_state.quantum_pause = false; qmp_cont(NULL); }
uint64_t* increment_total_num_instr(void) {
    quantum_state.total_num_instructions++;
    return &(quantum_state.total_num_instructions);
}
uint64_t query_total_num_instr(void)        { return quantum_state.total_num_instructions; }
uint64_t query_quantum_core_value(void)     { return quantum_state.quantum_value; }
uint64_t query_quantum_record_value(void)   { return quantum_state.quantum_record_value; }
uint64_t query_quantum_step_value(void)     { return quantum_state.quantum_step_value; }
uint64_t query_quantum_node_value(void)     { return quantum_state.quantum_node_value; }
const char* query_quantum_file_value(void)  { return quantum_state.quantum_file_value; }
void set_total_num_instr(uint64_t val)      { quantum_state.total_num_instructions = val; }
void set_quantum_value(uint64_t val)        { quantum_state.quantum_value = val; }
void set_quantum_record_value(uint64_t val) { quantum_state.quantum_record_value = val; }
void set_quantum_node_value(uint64_t val)   { quantum_state.quantum_node_value = val; }
#endif /* CONFIG_QUANTUM */

#ifdef CONFIG_EXTSNAP
#include <dirent.h>

typedef struct phases_state_t {
    uint64_t val;
    int id;
    QLIST_ENTRY(phases_state_t) next;
}phases_state_t;
typedef struct ckpt_state_t {
    uint64_t ckpt_interval;
    uint64_t ckpt_end;
    char* base_snap_name;
    int ckpt_id;
}ckpt_state_t;

static ckpt_state_t ckpt_state;
static bool using_phases;
static bool using_ckpt;
static char* phases_prefix;
static char* snap_name;
static QLIST_HEAD(, phases_state_t) phases_head = QLIST_HEAD_INITIALIZER(phases_head);
static bool cont_requested, save_requested, quit_requested;

static const char* get_phases_prefix(void) { return phases_prefix; }
static const char* get_base_ckpt_name(void) { return ckpt_state.base_snap_name; }
uint64_t get_ckpt_interval(void){ return ckpt_state.ckpt_interval; }
uint64_t get_ckpt_end(void)     { return ckpt_state.ckpt_end; }
const char* get_ckpt_name(void) { return snap_name; }
bool phase_is_valid(void)   { return (QLIST_EMPTY(&phases_head) ? false : true); }
bool is_phases_enabled(void){ return using_phases; }
bool is_ckpt_enabled(void)  { return using_ckpt; }
void toggle_phases_creation(void) { using_phases = !using_phases; }
void toggle_ckpt_creation(void)   { using_ckpt   = !using_ckpt; }
void toggle_save_request(void)  { save_requested = !save_requested; }
void toggle_cont_request(void)  { cont_requested = !cont_requested; }
void request_cont(void) { cont_requested = true; }
void request_quit(void) { quit_requested = true; }
bool save_request_pending(void) { return save_requested; }
bool cont_request_pending(void) { return cont_requested; }
bool quit_request_pending(void) { return quit_requested; }

static int get_phase_id(void)
{
    if (QLIST_EMPTY(&phases_head)) assert(false);
    phases_state_t *p = QLIST_FIRST(&phases_head);
    return p->id;
}
uint64_t get_phase_value(void){
    if (QLIST_EMPTY(&phases_head)) assert(false);
    phases_state_t *p = QLIST_FIRST(&phases_head);
    return p->val;
}
void set_base_ckpt_name(const char* str){
    if (strcmp(str,"")==0) {
        ckpt_state.base_snap_name = (char*)malloc(6*sizeof(char));
        for (int i =0; i<6;++i) {
            char randomletter = 'A' + (random() % 26);
            ckpt_state.base_snap_name[i] = randomletter;
        }
    } else {
        ckpt_state.base_snap_name = strdup(str);
    }
}
static inline void request_save(const char*str) {
    snap_name = strdup(str);
    save_requested = true;
}
void save_ckpt(void) {
    char* name = (char*)malloc(strlen(get_base_ckpt_name()) + 10*sizeof(char));
    sprintf(name, "%s_ckpt_%03d", get_base_ckpt_name(), ckpt_state.ckpt_id++);
    request_save(name);
}
void save_phase(void) {
    char* name = (char*)malloc(strlen(get_phases_prefix()) + 5*sizeof(char));
    sprintf(name, "%s_%03d", get_phases_prefix(), get_phase_id());
    request_save(name);
}
void pop_phase(void) {
    if (QLIST_EMPTY(&phases_head)) assert(false);
    phases_state_t *p = QLIST_FIRST(&phases_head);
    QLIST_REMOVE(p, next);

}
void configure_phases(QemuOpts *opts, Error **errp) {
    const char* step_opt, *name_opt;
    step_opt = qemu_opt_get(opts, "steps");
    name_opt = qemu_opt_get(opts, "name");

    int id = 0;
    if (!step_opt) {
        error_setg(errp, "no distances for phases defined");
    }
    if (!name_opt) {
        fprintf(stderr, "no naming prefix  given for phases option. will use prefix phase_00X");
        phases_prefix = strdup("phase");
    } else {
        phases_prefix = strdup(name_opt);
    }

    phases_state_t * head = calloc(1, sizeof(phases_state_t));

    char* token = strtok((char*) step_opt, ":");
    processForOpts(&head->val, token, errp);
    head->id = id++;
    QLIST_INSERT_HEAD(&phases_head, head, next);

    while (token) {
        token = strtok(NULL, ":");

        if (token) {
            phases_state_t* phase = calloc(1, sizeof(phases_state_t));
            processForOpts(&phase->val, token, errp);
            phase->id= id++;
            QLIST_INSERT_AFTER(head, phase, next);
            head = phase;
        }
    }
}
void configure_ckpt(QemuOpts *opts, Error **errp) {
    const char* every_opt, *end_opt;
    every_opt = qemu_opt_get(opts, "every");
    end_opt = qemu_opt_get(opts, "end");

    if (!every_opt) {
        error_setg(errp, "no interval given for ckpt option. cant continue");
    }
    if (!end_opt) {
        error_setg(errp, "no end given for ckpt option. cant continue");
    }

    processForOpts(&ckpt_state.ckpt_interval, every_opt, errp);
    processForOpts(&ckpt_state.ckpt_end, end_opt, errp);

    if (ckpt_state.ckpt_end < ckpt_state.ckpt_interval) {
        error_setg(errp, "ckpt end cant be smaller than ckpt interval");
    }
}
#endif /* CONFIG_EXTSNAP */

#ifdef CONFIG_LINUX

#include <sys/prctl.h>

#ifndef PR_MCE_KILL
#define PR_MCE_KILL 33
#endif

#ifndef PR_MCE_KILL_SET
#define PR_MCE_KILL_SET 1
#endif

#ifndef PR_MCE_KILL_EARLY
#define PR_MCE_KILL_EARLY 1
#endif

#endif /* CONFIG_LINUX */

int64_t max_delay;
int64_t max_advance;

/* vcpu throttling controls */
static QEMUTimer *throttle_timer;
static unsigned int throttle_percentage;

#define CPU_THROTTLE_PCT_MIN 1
#define CPU_THROTTLE_PCT_MAX 99
#define CPU_THROTTLE_TIMESLICE_NS 10000000

bool cpu_is_stopped(CPUState *cpu)
{
    return cpu->stopped || !runstate_is_running();
}

static bool cpu_thread_is_idle(CPUState *cpu)
{
    if (cpu->stop || cpu->queued_work_first) {
        return false;
    }
    if (cpu_is_stopped(cpu)) {
        return true;
    }
    if (!cpu->halted || cpu_has_work(cpu) ||
        kvm_halt_in_kernel()) {
        return false;
    }
    return true;
}

static bool all_cpu_threads_idle(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (!cpu_thread_is_idle(cpu)) {
            return false;
        }
    }
    return true;
}

/***********************************************************/
/* guest cycle counter */

/* Protected by TimersState seqlock */

static bool icount_sleep = true;
static int64_t vm_clock_warp_start = -1;
/* Conversion factor from emulated instructions to virtual clock ticks.  */
static int icount_time_shift;
/* Arbitrarily pick 1MIPS as the minimum allowable speed.  */
#define MAX_ICOUNT_SHIFT 10

static QEMUTimer *icount_rt_timer;
static QEMUTimer *icount_vm_timer;
static QEMUTimer *icount_warp_timer;

typedef struct TimersState {
    /* Protected by BQL.  */
    int64_t cpu_ticks_prev;
    int64_t cpu_ticks_offset;

    /* cpu_clock_offset can be read out of BQL, so protect it with
     * this lock.
     */
    QemuSeqLock vm_clock_seqlock;
    int64_t cpu_clock_offset;
    int32_t cpu_ticks_enabled;
    int64_t dummy;

    /* Compensate for varying guest execution speed.  */
    int64_t qemu_icount_bias;
    /* Only written by TCG thread */
    int64_t qemu_icount;

    /* Msutherl: State variable indicating icount subsection was received */
    bool icount_subsec_set;
} TimersState;

static TimersState timers_state;
bool mttcg_enabled;

/*
 * We default to false if we know other options have been enabled
 * which are currently incompatible with MTTCG. Otherwise when each
 * guest (target) has been updated to support:
 *   - atomic instructions
 *   - memory ordering primitives (barriers)
 * they can set the appropriate CONFIG flags in ${target}-softmmu.mak
 *
 * Once a guest architecture has been converted to the new primitives
 * there are two remaining limitations to check.
 *
 * - The guest can't be oversized (e.g. 64 bit guest on 32 bit host)
 * - The host must have a stronger memory order than the guest
 *
 * It may be possible in future to support strong guests on weak hosts
 * but that will require tagging all load/stores in a guest with their
 * implicit memory order requirements which would likely slow things
 * down a lot.
 */

static bool check_tcg_memory_orders_compatible(void)
{
#if defined(TCG_GUEST_DEFAULT_MO) && defined(TCG_TARGET_DEFAULT_MO)
    return (TCG_GUEST_DEFAULT_MO & ~TCG_TARGET_DEFAULT_MO) == 0;
#else
    return false;
#endif
}

static bool default_mttcg_enabled(void)
{
    if (use_icount || TCG_OVERSIZED_GUEST) {
        return false;
    } else {
#ifdef TARGET_SUPPORTS_MTTCG
        return check_tcg_memory_orders_compatible();
#else
        return false;
#endif
    }
}

void qemu_tcg_configure(QemuOpts *opts, Error **errp)
{
    const char *t = qemu_opt_get(opts, "thread");
    if (t) {
        if (strcmp(t, "multi") == 0) {
            if (TCG_OVERSIZED_GUEST) {
                error_setg(errp, "No MTTCG when guest word size > hosts");
            } else if (use_icount) {
                error_setg(errp, "No MTTCG when icount is enabled");
            } else {
#ifndef TARGET_SUPPORTS_MTTCG
                error_report("Guest not yet converted to MTTCG - "
                             "you may get unexpected results");
#endif
                if (!check_tcg_memory_orders_compatible()) {
                    error_report("Guest expects a stronger memory ordering "
                                 "than the host provides");
                    error_printf("This may cause strange/hard to debug errors\n");
                }
                mttcg_enabled = true;
            }
        } else if (strcmp(t, "single") == 0) {
            mttcg_enabled = false;
        } else {
            error_setg(errp, "Invalid 'thread' setting %s", t);
        }
    } else {
        mttcg_enabled = default_mttcg_enabled();
    }
}

/* The current number of executed instructions is based on what we
 * originally budgeted minus the current state of the decrementing
 * icount counters in extra/u16.low.
 */
static int64_t cpu_get_icount_executed(CPUState *cpu)
{
    return cpu->icount_budget - (cpu->icount_decr.u16.low + cpu->icount_extra);
}

/*
 * Update the global shared timer_state.qemu_icount to take into
 * account executed instructions. This is done by the TCG vCPU
 * thread so the main-loop can see time has moved forward.
 */
void cpu_update_icount(CPUState *cpu)
{
    int64_t executed = cpu_get_icount_executed(cpu);
    cpu->icount_budget -= executed;

#ifdef CONFIG_ATOMIC64
    atomic_set__nocheck(&timers_state.qemu_icount,
                        atomic_read__nocheck(&timers_state.qemu_icount) +
                        executed);
#else /* FIXME: we need 64bit atomics to do this safely */
    timers_state.qemu_icount += executed;
#endif
}

int64_t cpu_get_icount_raw(void)
{
    PTH_UPDATE_CONTEXT
    CPUState *cpu = PTH(current_cpu);

    if (cpu && cpu->running) {
        if (!cpu->can_do_io) {
            fprintf(stderr, "Bad icount read\n");
            exit(1);
        }
        /* Take into account what has run */
        cpu_update_icount(cpu);
    }
#ifdef CONFIG_ATOMIC64
    return atomic_read__nocheck(&timers_state.qemu_icount);
#else /* FIXME: we need 64bit atomics to do this safely */
    return timers_state.qemu_icount;
#endif
}

/* Return the virtual CPU time, based on the instruction counter.  */
static int64_t cpu_get_icount_locked(void)
{
    int64_t icount = cpu_get_icount_raw();
    return timers_state.qemu_icount_bias + cpu_icount_to_ns(icount);
}

int64_t cpu_get_icount(void)
{
    int64_t icount;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        icount = cpu_get_icount_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return icount;
}

int64_t cpu_icount_to_ns(int64_t icount)
{
    return icount << icount_time_shift;
}

/* return the time elapsed in VM between vm_start and vm_stop.  Unless
 * icount is active, cpu_get_ticks() uses units of the host CPU cycle
 * counter.
 *
 * Caller must hold the BQL
 */
int64_t cpu_get_ticks(void)
{
    int64_t ticks;

    if (use_icount) {
        return cpu_get_icount();
    }

    ticks = timers_state.cpu_ticks_offset;
    if (timers_state.cpu_ticks_enabled) {
        ticks += cpu_get_host_ticks();
    }

    if (timers_state.cpu_ticks_prev > ticks) {
        /* Note: non increasing ticks may happen if the host uses
           software suspend */
        timers_state.cpu_ticks_offset += timers_state.cpu_ticks_prev - ticks;
        ticks = timers_state.cpu_ticks_prev;
    }

    timers_state.cpu_ticks_prev = ticks;
    return ticks;
}

static int64_t cpu_get_clock_locked(void)
{
    int64_t time;

    time = timers_state.cpu_clock_offset;
    if (timers_state.cpu_ticks_enabled) {
        time += get_clock();
    }

    return time;
}

/* Return the monotonic time elapsed in VM, i.e.,
 * the time between vm_start and vm_stop
 */
int64_t cpu_get_clock(void)
{
    int64_t ti;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        ti = cpu_get_clock_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return ti;
}

/* enable cpu_get_ticks()
 * Caller must hold BQL which serves as mutex for vm_clock_seqlock.
 */
void cpu_enable_ticks(void)
{
    /* Here, the really thing protected by seqlock is cpu_clock_offset. */
    seqlock_write_begin(&timers_state.vm_clock_seqlock);
    if (!timers_state.cpu_ticks_enabled) {
        timers_state.cpu_ticks_offset -= cpu_get_host_ticks();
        timers_state.cpu_clock_offset -= get_clock();
        timers_state.cpu_ticks_enabled = 1;
    }
    seqlock_write_end(&timers_state.vm_clock_seqlock);
}

/* disable cpu_get_ticks() : the clock is stopped. You must not call
 * cpu_get_ticks() after that.
 * Caller must hold BQL which serves as mutex for vm_clock_seqlock.
 */
void cpu_disable_ticks(void)
{
    /* Here, the really thing protected by seqlock is cpu_clock_offset. */
    seqlock_write_begin(&timers_state.vm_clock_seqlock);
    if (timers_state.cpu_ticks_enabled) {
        timers_state.cpu_ticks_offset += cpu_get_host_ticks();
        timers_state.cpu_clock_offset = cpu_get_clock_locked();
        timers_state.cpu_ticks_enabled = 0;
    }
    seqlock_write_end(&timers_state.vm_clock_seqlock);
}

/* Correlation between real and virtual time is always going to be
   fairly approximate, so ignore small variation.
   When the guest is idle real and virtual time will be aligned in
   the IO wait loop.  */
#define ICOUNT_WOBBLE (NANOSECONDS_PER_SECOND / 10)

static void icount_adjust(void)
{
    int64_t cur_time;
    int64_t cur_icount;
    int64_t delta;

    /* Protected by TimersState mutex.  */
    static int64_t last_delta;

    /* If the VM is not running, then do nothing.  */
    if (!runstate_is_running()) {
        return;
    }

    seqlock_write_begin(&timers_state.vm_clock_seqlock);
    cur_time = cpu_get_clock_locked();
    cur_icount = cpu_get_icount_locked();

    delta = cur_icount - cur_time;
    /* FIXME: This is a very crude algorithm, somewhat prone to oscillation.  */
    if (delta > 0
        && last_delta + ICOUNT_WOBBLE < delta * 2
        && icount_time_shift > 0) {
        /* The guest is getting too far ahead.  Slow time down.  */
        icount_time_shift--;
    }
    if (delta < 0
        && last_delta - ICOUNT_WOBBLE > delta * 2
        && icount_time_shift < MAX_ICOUNT_SHIFT) {
        /* The guest is getting too far behind.  Speed time up.  */
        icount_time_shift++;
    }
    last_delta = delta;
    timers_state.qemu_icount_bias = cur_icount
                              - (timers_state.qemu_icount << icount_time_shift);
    seqlock_write_end(&timers_state.vm_clock_seqlock);
}

static void icount_adjust_rt(void *opaque)
{
    timer_mod(icount_rt_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    icount_adjust();
}

static void icount_adjust_vm(void *opaque)
{
    timer_mod(icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
    icount_adjust();
}

static int64_t qemu_icount_round(int64_t count)
{
    return (count + (1 << icount_time_shift) - 1) >> icount_time_shift;
}

static void icount_warp_rt(void)
{
    unsigned seq;
    int64_t warp_start;

    /* The icount_warp_timer is rescheduled soon after vm_clock_warp_start
     * changes from -1 to another value, so the race here is okay.
     */
    do {
        seq = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        warp_start = vm_clock_warp_start;
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, seq));

    if (warp_start == -1) {
        return;
    }

    seqlock_write_begin(&timers_state.vm_clock_seqlock);
    if (runstate_is_running()) {
        int64_t clock = REPLAY_CLOCK(REPLAY_CLOCK_VIRTUAL_RT,
                                     cpu_get_clock_locked());
        int64_t warp_delta;

        warp_delta = clock - vm_clock_warp_start;
        if (use_icount == 2) {
            /*
             * In adaptive mode, do not let QEMU_CLOCK_VIRTUAL run too
             * far ahead of real time.
             */
            int64_t cur_icount = cpu_get_icount_locked();
            int64_t delta = clock - cur_icount;
            warp_delta = MIN(warp_delta, delta);
        }
        timers_state.qemu_icount_bias += warp_delta;
    }
    vm_clock_warp_start = -1;
    seqlock_write_end(&timers_state.vm_clock_seqlock);

    if (qemu_clock_expired(QEMU_CLOCK_VIRTUAL)) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}

static void icount_timer_cb(void *opaque)
{
    /* No need for a checkpoint because the timer already synchronizes
     * with CHECKPOINT_CLOCK_VIRTUAL_RT.
     */
    icount_warp_rt();
}

void qtest_clock_warp(int64_t dest)
{
    int64_t clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    AioContext *aio_context;
    assert(qtest_enabled());
    aio_context = qemu_get_aio_context();
    while (clock < dest) {
        int64_t deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL);
        int64_t warp = qemu_soonest_timeout(dest - clock, deadline);

        seqlock_write_begin(&timers_state.vm_clock_seqlock);
        timers_state.qemu_icount_bias += warp;
        seqlock_write_end(&timers_state.vm_clock_seqlock);

        qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
        timerlist_run_timers(aio_context->tlg.tl[QEMU_CLOCK_VIRTUAL]);
        clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
}

void qemu_start_warp_timer(void)
{
    int64_t clock;
    int64_t deadline;

    if (!use_icount) {
        return;
    }

    /* Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        return;
    }

    /* warp clock deterministically in record/replay mode */
    if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_START)) {
        return;
    }

    if (!all_cpu_threads_idle()) {
        return;
    }

    if (qtest_enabled()) {
        /* When testing, qtest commands advance icount.  */
        return;
    }

    /* We want to use the earliest deadline from ALL vm_clocks */
    clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
    deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL);
    if (deadline < 0) {
        static bool notified;
        if (!icount_sleep && !notified) {
            warn_report("icount sleep disabled and no active timers");
            notified = true;
        }
        return;
    }

    if (deadline > 0) {
        /*
         * Ensure QEMU_CLOCK_VIRTUAL proceeds even when the virtual CPU goes to
         * sleep.  Otherwise, the CPU might be waiting for a future timer
         * interrupt to wake it up, but the interrupt never comes because
         * the vCPU isn't running any insns and thus doesn't advance the
         * QEMU_CLOCK_VIRTUAL.
         */
        if (!icount_sleep) {
            /*
             * We never let VCPUs sleep in no sleep icount mode.
             * If there is a pending QEMU_CLOCK_VIRTUAL timer we just advance
             * to the next QEMU_CLOCK_VIRTUAL event and notify it.
             * It is useful when we want a deterministic execution time,
             * isolated from host latencies.
             */
            seqlock_write_begin(&timers_state.vm_clock_seqlock);
            timers_state.qemu_icount_bias += deadline;
            seqlock_write_end(&timers_state.vm_clock_seqlock);
            qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
        } else {
            /*
             * We do stop VCPUs and only advance QEMU_CLOCK_VIRTUAL after some
             * "real" time, (related to the time left until the next event) has
             * passed. The QEMU_CLOCK_VIRTUAL_RT clock will do this.
             * This avoids that the warps are visible externally; for example,
             * you will not be sending network packets continuously instead of
             * every 100ms.
             */
            seqlock_write_begin(&timers_state.vm_clock_seqlock);
            if (vm_clock_warp_start == -1 || vm_clock_warp_start > clock) {
                vm_clock_warp_start = clock;
            }
            seqlock_write_end(&timers_state.vm_clock_seqlock);
            timer_mod_anticipate(icount_warp_timer, clock + deadline);
        }
    } else if (deadline == 0) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}

static void qemu_account_warp_timer(void)
{
    if (!use_icount || !icount_sleep) {
        return;
    }

    /* Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        return;
    }

    /* warp clock deterministically in record/replay mode */
    if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_ACCOUNT)) {
        return;
    }

    timer_del(icount_warp_timer);
    icount_warp_rt();
}

static bool icount_state_needed(void *opaque)
{
    return use_icount;
}

static int icount_timers_post_load(void *opaque, int version_id) {
    TimersState *s = (TimersState*) opaque;
    s->icount_subsec_set = true;
    return 0;
}

static int timers_global_pre_load(void *opaque) {
    TimersState *s = (TimersState*) opaque;
    s->icount_subsec_set = false;
    return 0;
}

static int timers_global_post_load(void *opaque,int version_id) {
    TimersState *s = (TimersState*) opaque;
    if (use_icount && !s->icount_subsec_set) {
        timers_state.qemu_icount_bias = cpu_get_clock();
    }
    return 0;
}

/*
 * This is a subsection for icount migration.
 */
static const VMStateDescription icount_vmstate_timers = {
    .name = "timer/icount",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = icount_state_needed,
    .post_load = icount_timers_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_INT64(qemu_icount_bias, TimersState),
        VMSTATE_INT64(qemu_icount, TimersState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_timers = {
    .name = "timer",
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_load = timers_global_pre_load,
    .post_load = timers_global_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_INT64(cpu_ticks_offset, TimersState),
        VMSTATE_INT64(dummy, TimersState),
        VMSTATE_INT64_V(cpu_clock_offset, TimersState, 2),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription*[]) {
        &icount_vmstate_timers,
        NULL
    }
};

static void cpu_throttle_thread(CPUState *cpu, run_on_cpu_data opaque)
{
    double pct;
    double throttle_ratio;
    long sleeptime_ns;

    if (!cpu_throttle_get_percentage()) {
        return;
    }

    pct = (double)cpu_throttle_get_percentage()/100;
    throttle_ratio = pct / (1 - pct);
    sleeptime_ns = (long)(throttle_ratio * CPU_THROTTLE_TIMESLICE_NS);

    qemu_mutex_unlock_iothread();
    portable_usleep(sleeptime_ns / 1000); /* Convert ns to us for usleep call */
    qemu_mutex_lock_iothread();
    atomic_set(&cpu->throttle_thread_scheduled, 0);
}

static void cpu_throttle_timer_tick(void *opaque)
{
    CPUState *cpu;
    double pct;

    /* Stop the timer if needed */
    if (!cpu_throttle_get_percentage()) {
        return;
    }
    CPU_FOREACH(cpu) {
        if (!atomic_xchg(&cpu->throttle_thread_scheduled, 1)) {
            async_run_on_cpu(cpu, cpu_throttle_thread,
                             RUN_ON_CPU_NULL);
        }
    }

    pct = (double)cpu_throttle_get_percentage()/100;
    timer_mod(throttle_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT) +
                                   CPU_THROTTLE_TIMESLICE_NS / (1-pct));
}

void cpu_throttle_set(int new_throttle_pct)
{
    /* Ensure throttle percentage is within valid range */
    new_throttle_pct = MIN(new_throttle_pct, CPU_THROTTLE_PCT_MAX);
    new_throttle_pct = MAX(new_throttle_pct, CPU_THROTTLE_PCT_MIN);

    atomic_set(&throttle_percentage, new_throttle_pct);

    timer_mod(throttle_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT) +
                                       CPU_THROTTLE_TIMESLICE_NS);
}

void cpu_throttle_stop(void)
{
    atomic_set(&throttle_percentage, 0);
}

bool cpu_throttle_active(void)
{
    return (cpu_throttle_get_percentage() != 0);
}

int cpu_throttle_get_percentage(void)
{
    return atomic_read(&throttle_percentage);
}

void cpu_ticks_init(void)
{
    seqlock_init(&timers_state.vm_clock_seqlock);
    vmstate_register(NULL, 0, &vmstate_timers, &timers_state);
    throttle_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL_RT,
                                           cpu_throttle_timer_tick, NULL);
}

#if defined(CONFIG_QUANTUM) || defined(CONFIG_FLEXUS) || defined(CONFIG_EXTSNAP)
#define KIL 1E3
#define MIL 1E6
#define BIL 1E9
void processLetterforExponent(uint64_t *val, char c, Error **errp)
{
    switch(c) {
        case 'K': case 'k' :
        *val *= KIL;
        break;
        case 'M':case 'm':
        *val  *= MIL;
        break;
        case 'B':case 'b':
        *val  *= BIL;
        break;
        default:
        error_setg(errp, "the suffix you used is not valid: valid suffixes are K,k,M,m,B,b");
        exit(1);
        break;
    }
}
void processForOpts(uint64_t *val, const char* qopt, Error **errp)
{
    size_t s = strlen(qopt);
    char c = qopt[s-1];

    if (isalpha(c)) {
        char* temp= strndup(qopt,  strlen(qopt)-1);
        *val = atoi(temp);
        free(temp);
        if (*val <= 0){
            *val = 0;
            return;
        }

        processLetterforExponent(&(*val), c, errp);
    } else {
        *val = atoi(qopt);
    }
}
#endif

#ifdef CONFIG_QUANTUM
void configure_quantum(QemuOpts *opts, Error **errp)
{
    const char* qopt, *qopt_record, *qopt_node, *qopt_step, *qopt_file;
    qopt = qemu_opt_get(opts, "core");
    qopt_record = qemu_opt_get(opts, "record");
    qopt_step = qemu_opt_get(opts, "step");
    qopt_file = qemu_opt_get(opts, "file");
    qopt_node = qemu_opt_get(opts, "node");

    if (!qopt_file && qopt_record) {
        fprintf(stderr, "no file defined for quantum record output - using current directory and will save to quantum_file.dat");
        quantum_state.quantum_file_value = strdup("quantum_file.dat"); // defaulting to current directory
    } else if (qopt_file && !qopt_record) {
        error_setg(errp, "quantum step can only be used with record");
    } else if (qopt_file && qopt_record) {
        quantum_state.quantum_file_value = strdup(qopt_file);
    }

    if (!qopt_step && qopt_record) {
        fprintf(stderr, "no quantum step defined for quantum record. - will use a default value of 100M");
        quantum_state.quantum_step_value = 1e8; // 100 MIPS by default
    } else if (qopt_step && !qopt_record) {
        error_setg(errp, "quantum step can only be used with record");
    } else if (qopt_step && qopt_record) {
        processForOpts(&quantum_state.quantum_step_value, qopt_step, errp);
    }

    if (!qopt && !qopt_record && !qopt_node) {
        error_setg(errp, "quantum option is not valid");
        exit(1);
    }

    if (qopt) {
        if (qemu_tcg_mttcg_enabled()) {
            error_setg(errp, "Quantum is not supported with mttcg. Use -accel tcg,thread=single or -icount.\n");
            exit(1);
        } else {
            processForOpts(&quantum_state.quantum_value, qopt, errp);
            if (!qemu_loglevel_mask(CPU_LOG_TB_NOCHAIN)) {
                printf("quantum value is not guaranteed to work with chaining TBs. use '-d nochain'\n");
            }
        }
    }

    if (qopt_record) {
        processForOpts(&quantum_state.quantum_record_value, qopt_record, errp);
    }
    if (qopt_node) {
        processForOpts(&quantum_state.quantum_node_value, qopt_node, errp);
        if (quantum_state.quantum_node_value > 0) {
           raise(SIGSTOP);
        }
    }
}
#endif /* CONFIG_QUANTUM */

#ifdef CONFIG_FLEXUS
void configure_flexus(QemuOpts *opts, Error **errp)
{
    const char* mode_opt, *length_opt, *simulator_opt, *config_opt,*debug_opt;
    mode_opt = qemu_opt_get(opts, "mode");
    length_opt = qemu_opt_get(opts, "length");
    simulator_opt = qemu_opt_get(opts, "simulator");
    config_opt = qemu_opt_get(opts, "config");
    debug_opt = qemu_opt_get(opts, "debug");

    if (!mode_opt) {
        error_setg(errp, "simulator mode needs to be defined");
    }
    char* temp = strdup(mode_opt);
    /* Set trace or timing */
    flexus_state.mode = strcmp(temp, "timing")==0 ? TIMING : TRACE;

    /* Parse MANDATORY simulation length, simulator, and config options */
    if (!length_opt || !simulator_opt || !config_opt) {
        error_setg(errp, "ERROR: Not all flexus options need to be defined. Mandatory: length=,simulator=,config=");
    }
    processForOpts(&flexus_state.length, length_opt, errp);
    if (flexus_state.length == 0) {
        error_setg(errp, "Refusing to run with ZERO simulation length.");
    }
    QEMU_setSimulationTime(flexus_state.length);
    QEMU_initialize((flexus_state.mode == TIMING) ? true : false);
    free(temp);

    flexus_state.simulator = strdup(simulator_opt);
    if( access( flexus_state.simulator, F_OK ) != -1 ) {
        flexus_state.simulator_obj = simulator_load( flexus_state.simulator );
        if (flexus_state.simulator_obj){
            fprintf(stderr, "<%s:%i> Flexus Simulator set!.\n", basename(__FILE__), __LINE__);
        } else {
            error_setg(errp, "simulator could not be set.!.\n");
        }
    } else {
        error_setg(errp, "simulator path contains no simulator!.\n");
    }

    flexus_state.config_file = strdup(config_opt);
    if(! (access( flexus_state.config_file, F_OK ) != -1) ) {
        error_setg(errp, "no config file (user_postload) at this path %s\n", config_opt);
    }

    if (debug_opt) {
        char* tmp = strdup(debug_opt);
        for (int i = 0; i < strlen(debug_opt); i++){
            tmp[i] = tolower(debug_opt[i]);
        }

        flexus_state.debug_mode = strdup(tmp);

        free(tmp);
    }

    initFlexus();

    // trigger the periodic event
    QEMU_execute_callbacks(-1, 0, 0);
}
void set_flexus_load_dir(const char* dir_name) {
    DIR* dir = opendir(dir_name);
    if (dir) {
        /* Directory exists. */
        flexus_state.load_dir = strdup(dir_name);
        closedir(dir);
    } else if (ENOENT == errno) {
        /* Directory does not exist. */
    } else {
        /* opendir() failed for some other reason. */
    }
}
#endif /* CONFIG_FLEXUS */

void configure_icount(QemuOpts *opts, Error **errp)
{
    const char *option;
    char *rem_str = NULL;

    option = qemu_opt_get(opts, "shift");
    if (!option) {
        if (qemu_opt_get(opts, "align") != NULL) {
            error_setg(errp, "Please specify shift option when using align");
        }
        return;
    }

    icount_sleep = qemu_opt_get_bool(opts, "sleep", true);
    if (icount_sleep) {
        icount_warp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL_RT,
                                         icount_timer_cb, NULL);
    }

    icount_align_option = qemu_opt_get_bool(opts, "align", false);

    if (icount_align_option && !icount_sleep) {
        error_setg(errp, "align=on and sleep=off are incompatible");
    }
    if (strcmp(option, "auto") != 0) {
        errno = 0;
        icount_time_shift = strtol(option, &rem_str, 0);
        if (errno != 0 || *rem_str != '\0' || !strlen(option)) {
            error_setg(errp, "icount: Invalid shift value");
        }
        use_icount = 1;
        return;
    } else if (icount_align_option) {
        error_setg(errp, "shift=auto and align=on are incompatible");
    } else if (!icount_sleep) {
        error_setg(errp, "shift=auto and sleep=off are incompatible");
    }

    use_icount = 2;

    /* 125MIPS seems a reasonable initial guess at the guest speed.
       It will be corrected fairly quickly anyway.  */
    icount_time_shift = 3;

    /* Have both realtime and virtual time triggers for speed adjustment.
       The realtime trigger catches emulated time passing too slowly,
       the virtual time trigger catches emulated time passing too fast.
       Realtime triggers occur even when idle, so use them less frequently
       than VM triggers.  */
    icount_rt_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL_RT,
                                   icount_adjust_rt, NULL);
    timer_mod(icount_rt_timer,
                   qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    icount_vm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        icount_adjust_vm, NULL);
    timer_mod(icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
}

/***********************************************************/
/* TCG vCPU kick timer
 *
 * The kick timer is responsible for moving single threaded vCPU
 * emulation on to the next vCPU. If more than one vCPU is running a
 * timer event with force a cpu->exit so the next vCPU can get
 * scheduled.
 *
 * The timer is removed if all vCPUs are idle and restarted again once
 * idleness is complete.
 */

static QEMUTimer *tcg_kick_vcpu_timer;
static CPUState *tcg_current_rr_cpu;

#define TCG_KICK_PERIOD (NANOSECONDS_PER_SECOND / 10)

static inline int64_t qemu_tcg_next_kick(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TCG_KICK_PERIOD;
}

/* Kick the currently round-robin scheduled vCPU */
static void qemu_cpu_kick_rr_cpu(void)
{
    CPUState *cpu;
    do {
        cpu = atomic_mb_read(&tcg_current_rr_cpu);
        if (cpu) {
            cpu_exit(cpu);
        }
    } while (cpu != atomic_mb_read(&tcg_current_rr_cpu));
}

static void do_nothing(CPUState *cpu, run_on_cpu_data unused)
{
}

void qemu_timer_notify_cb(void *opaque, QEMUClockType type)
{
    if (!use_icount || type != QEMU_CLOCK_VIRTUAL) {
        qemu_notify_event();
        return;
    }

    if (!qemu_in_vcpu_thread() && first_cpu) {
        /* qemu_cpu_kick is not enough to kick a halted CPU out of
         * qemu_tcg_wait_io_event.  async_run_on_cpu, instead,
         * causes cpu_thread_is_idle to return false.  This way,
         * handle_icount_deadline can run.
         */
        async_run_on_cpu(first_cpu, do_nothing, RUN_ON_CPU_NULL);
    }
}

static void kick_tcg_thread(void *opaque)
{
    timer_mod(tcg_kick_vcpu_timer, qemu_tcg_next_kick());
    qemu_cpu_kick_rr_cpu();
}

static void start_tcg_kick_timer(void)
{
    if (!mttcg_enabled && !tcg_kick_vcpu_timer && CPU_NEXT(first_cpu)) {
        tcg_kick_vcpu_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           kick_tcg_thread, NULL);
        timer_mod(tcg_kick_vcpu_timer, qemu_tcg_next_kick());
    }
}

static void stop_tcg_kick_timer(void)
{
    if (tcg_kick_vcpu_timer) {
        timer_del(tcg_kick_vcpu_timer);
        tcg_kick_vcpu_timer = NULL;
    }
}

/***********************************************************/
void hw_error(const char *fmt, ...)
{
    va_list ap;
    CPUState *cpu;

    va_start(ap, fmt);
    fprintf(stderr, "qemu: hardware error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    CPU_FOREACH(cpu) {
        fprintf(stderr, "CPU #%d:\n", cpu->cpu_index);
        cpu_dump_state(cpu, stderr, fprintf, CPU_DUMP_FPU);
    }
    va_end(ap);
    abort();
}

void cpu_synchronize_all_states(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_state(cpu);
    }
}

void cpu_synchronize_all_post_reset(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_post_reset(cpu);
    }
}

void cpu_synchronize_all_post_init(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_post_init(cpu);
    }
}

void cpu_synchronize_all_pre_loadvm(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_pre_loadvm(cpu);
    }
}

static int do_vm_stop(RunState state)
{
    int ret = 0;

    if (runstate_is_running()) {
        cpu_disable_ticks();
        pause_all_vcpus();
        runstate_set(state);
        vm_state_notify(0, state);
        qapi_event_send_stop(&error_abort);
    }

    bdrv_drain_all();
    replay_disable_events();
    ret = bdrv_flush_all();

    return ret;
}

static bool cpu_can_run(CPUState *cpu)
{
    if (cpu->stop) {
        return false;
    }
    if (cpu_is_stopped(cpu)) {
        return false;
    }
    return true;
}

static void cpu_handle_guest_debug(CPUState *cpu)
{
    gdb_set_stop_cpu(cpu);
    qemu_system_debug_request();
    cpu->stopped = true;
}

#ifdef CONFIG_LINUX
static void sigbus_reraise(void)
{
    sigset_t set;
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    if (!sigaction(SIGBUS, &action, NULL)) {
        raise(SIGBUS);
        sigemptyset(&set);
        sigaddset(&set, SIGBUS);
#ifndef CONFIG_PTH
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
#else
        pthpthread_sigmask(SIG_UNBLOCK, &set, NULL);
#endif
    }
    perror("Failed to re-raise SIGBUS!\n");
    abort();
}

static void sigbus_handler(int n, siginfo_t *siginfo, void *ctx)
{
    PTH_UPDATE_CONTEXT
    if (siginfo->si_code != BUS_MCEERR_AO && siginfo->si_code != BUS_MCEERR_AR) {
        sigbus_reraise();
    }
    if (PTH(current_cpu)) {
        /* Called asynchronously in VCPU thread.  */
        if (kvm_on_sigbus_vcpu(PTH(current_cpu), siginfo->si_code, siginfo->si_addr)) {
            sigbus_reraise();
        }
    } else {
        /* Called synchronously (via signalfd) in main thread.  */
        if (kvm_on_sigbus(siginfo->si_code, siginfo->si_addr)) {
            sigbus_reraise();
        }
    }
}

static void qemu_init_sigbus(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_flags = SA_SIGINFO;
    action.sa_sigaction = sigbus_handler;
    sigaction(SIGBUS, &action, NULL);

    prctl(PR_MCE_KILL, PR_MCE_KILL_SET, PR_MCE_KILL_EARLY, 0, 0);
}
#else /* !CONFIG_LINUX */
static void qemu_init_sigbus(void)
{
}
#endif /* !CONFIG_LINUX */

static QemuMutex qemu_global_mutex;

static QemuThread io_thread;

/* cpu creation */
static QemuCond qemu_cpu_cond;
/* system init */
static QemuCond qemu_pause_cond;

void qemu_init_cpu_loop(void)
{
    qemu_init_sigbus();
    qemu_cond_init(&qemu_cpu_cond);
    qemu_cond_init(&qemu_pause_cond);
    qemu_mutex_init(&qemu_global_mutex);

    qemu_thread_get_self(&io_thread);
}

void run_on_cpu(CPUState *cpu, run_on_cpu_func func, run_on_cpu_data data)
{
    do_run_on_cpu(cpu, func, data, &qemu_global_mutex);
}

static void qemu_kvm_destroy_vcpu(CPUState *cpu)
{
    if (kvm_destroy_vcpu(cpu) < 0) {
        error_report("kvm_destroy_vcpu failed");
        exit(EXIT_FAILURE);
    }
}

static void qemu_tcg_destroy_vcpu(CPUState *cpu)
{
}

static void qemu_wait_io_event_common(CPUState *cpu)
{
    atomic_mb_set(&cpu->thread_kicked, false);
    if (cpu->stop) {
        cpu->stop = false;
        cpu->stopped = true;
        qemu_cond_broadcast(&qemu_pause_cond);
    }
    process_queued_cpu_work(cpu);
}

static bool qemu_tcg_should_sleep(CPUState *cpu)
{
    if (mttcg_enabled) {
        return cpu_thread_is_idle(cpu);
    } else {
        return all_cpu_threads_idle();
    }
}

static void qemu_tcg_wait_io_event(CPUState *cpu)
{
    while (qemu_tcg_should_sleep(cpu)) {
        stop_tcg_kick_timer();
        qemu_cond_wait(cpu->halt_cond, &qemu_global_mutex);
    }

    start_tcg_kick_timer();

    qemu_wait_io_event_common(cpu);
}

static void qemu_kvm_wait_io_event(CPUState *cpu)
{
    while (cpu_thread_is_idle(cpu)) {
        qemu_cond_wait(cpu->halt_cond, &qemu_global_mutex);
    }

    qemu_wait_io_event_common(cpu);
}

static void *qemu_kvm_cpu_thread_fn(void *arg)
{
    PTH_UPDATE_CONTEXT
    CPUState *cpu = arg;
    int r;

    rcu_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;
    PTH(current_cpu) = cpu;
    r = kvm_init_vcpu(cpu);
    if (r < 0) {
        fprintf(stderr, "kvm_init_vcpu failed: %s\n", strerror(-r));
        exit(1);
    }

    kvm_init_cpu_signals(cpu);

    /* signal CPU creation */
    cpu->created = true;
    qemu_cond_signal(&qemu_cpu_cond);

    do {
        if (cpu_can_run(cpu)) {
            r = kvm_cpu_exec(cpu);
            if (r == EXCP_DEBUG) {
                cpu_handle_guest_debug(cpu);
            }
        }
        qemu_kvm_wait_io_event(cpu);
    } while (!cpu->unplug || cpu_can_run(cpu));

    qemu_kvm_destroy_vcpu(cpu);
    cpu->created = false;
    qemu_cond_signal(&qemu_cpu_cond);
    qemu_mutex_unlock_iothread();
    return NULL;
}

static void *qemu_dummy_cpu_thread_fn(void *arg)
{
#ifdef _WIN32
    fprintf(stderr, "qtest is not supported under Windows\n");
    exit(1);
#else
    PTH_UPDATE_CONTEXT
    CPUState *cpu = arg;
    sigset_t waitset;
    int r;

    rcu_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;
    PTH(current_cpu) = cpu;
    sigemptyset(&waitset);
    sigaddset(&waitset, SIG_IPI);

    /* signal CPU creation */
    cpu->created = true;
    qemu_cond_signal(&qemu_cpu_cond);

    while (1) {
        qemu_mutex_unlock_iothread();
        do {
            int sig;
            r = sigwait(&waitset, &sig);
        } while (r == -1 && (errno == EAGAIN || errno == EINTR));
        if (r == -1) {
            perror("sigwait");
            exit(1);
        }
        qemu_mutex_lock_iothread();
        qemu_wait_io_event_common(cpu);
    }

    return NULL;
#endif
}

static int64_t tcg_get_icount_limit(void)
{
    int64_t deadline;

    if (replay_mode != REPLAY_MODE_PLAY) {
        deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL);

        /* Maintain prior (possibly buggy) behaviour where if no deadline
         * was set (as there is no QEMU_CLOCK_VIRTUAL timer) or it is more than
         * INT32_MAX nanoseconds ahead, we still use INT32_MAX
         * nanoseconds.
         */
        if ((deadline < 0) || (deadline > INT32_MAX)) {
            deadline = INT32_MAX;
        }
#ifdef CONFIG_QUANTUM
        int64_t quantum_val = query_quantum_core_value();
        if (quantum_val > 0) { // assume quantum is enabled
            int64_t icount_scaled = qemu_icount_round(deadline);
            return (icount_scaled < quantum_val) ? icount_scaled : quantum_val;
        } else {
            return qemu_icount_round(deadline);
        }
#else
        return qemu_icount_round(deadline);
#endif
    } else {
        return replay_get_instructions();
    }
}

static void handle_icount_deadline(void)
{
    assert(qemu_in_vcpu_thread());
    if (use_icount) {
        int64_t deadline =
            qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL);

        if (deadline == 0) {
            /* Wake up other AioContexts.  */
            qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
            qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
        }
    }
}

static void prepare_icount_for_run(CPUState *cpu)
{
    if (use_icount) {
        int insns_left;

        /* These should always be cleared by process_icount_data after
         * each vCPU execution. However u16.high can be raised
         * asynchronously by cpu_exit/cpu_interrupt/tcg_handle_interrupt
         */
        g_assert(cpu->icount_decr.u16.low == 0);
        g_assert(cpu->icount_extra == 0);

        cpu->icount_budget = tcg_get_icount_limit();
        insns_left = MIN(0xffff, cpu->icount_budget);
        cpu->icount_decr.u16.low = insns_left;
        cpu->icount_extra = cpu->icount_budget - insns_left;
    }
}

static void process_icount_data(CPUState *cpu)
{
    if (use_icount) {
        /* Account for executed instructions */
        cpu_update_icount(cpu);

        /* Reset the counters */
        cpu->icount_decr.u16.low = 0;
        cpu->icount_extra = 0;
        cpu->icount_budget = 0;

        replay_account_executed_instructions();
    }
}

static int tcg_cpu_exec(CPUState *cpu)
{
    int ret;
#ifdef CONFIG_PROFILER
    int64_t ti;
    ti = profile_getclock();
#endif
    qemu_mutex_unlock_iothread();
    cpu_exec_start(cpu);
    ret = cpu_exec(cpu);
    cpu_exec_end(cpu);
    qemu_mutex_lock_iothread();
#ifdef CONFIG_PROFILER
    tcg_time += profile_getclock() - ti;
#endif
    return ret;
}


#if defined(CONFIG_FLEXUS)

static int qflex_tcg_cpu_exec(CPUState *cpu, QFlexExecType_t type)
{
    int ret;
#ifdef CONFIG_PROFILER
    int64_t ti;
    ti = profile_getclock();
#endif
    qemu_mutex_unlock_iothread();
    cpu_exec_start(cpu);
    ret = qflex_cpu_exec(cpu,type);
    cpu_exec_end(cpu);
    qemu_mutex_lock_iothread();
#ifdef CONFIG_PROFILER
    tcg_time += profile_getclock() - ti;
#endif
    return ret;
}

int qflex_cpu_step(CPUState *cpu, QFlexExecType_t type)
{
    int r = 0;

    /* Account partial waits to QEMU_CLOCK_VIRTUAL.  */
    qemu_account_warp_timer();

    /* Run the timers here.  This is much more efficient than
     * waking up the I/O thread and waiting for completion.
     */
    handle_icount_deadline();

    if (!cpu->queued_work_first && !cpu->exit_request) {
        atomic_mb_set(&tcg_current_rr_cpu, cpu);
        PTH_UPDATE_CONTEXT
        PTH(current_cpu) = cpu;

        qemu_clock_enable(QEMU_CLOCK_VIRTUAL,
                          (cpu->singlestep_enabled & SSTEP_NOTIMER) == 0);

        if (cpu_can_run(cpu)) {

            prepare_icount_for_run(cpu);
            r = qflex_tcg_cpu_exec(cpu, type);
            process_icount_data(cpu);

            switch (r) {
            case EXCP_DEBUG:
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:
                /* during start-up the vCPU is reset and the thread is
                 * kicked several times. If we don't ensure we go back
                 * to sleep in the halted state we won't cleanly
                 * start-up when the vCPU is enabled.
                 *
                 * cpu->halted should ensure we sleep in wait_io_event
                 */
                g_assert(cpu->halted);
                break;
            case EXCP_ATOMIC:
                qemu_mutex_unlock_iothread();
                cpu_exec_step_atomic(cpu);
                qemu_mutex_lock_iothread();
            default:
                /* Ignore everything else? */
                break;
            }
        } else if (cpu->unplug) {
            qemu_tcg_destroy_vcpu(cpu);
            cpu->created = false;
            qemu_cond_signal(&qemu_cpu_cond);
            qemu_mutex_unlock_iothread();
            /* tcg_cpu_exec() output is defined in cpu-all.h
             * if qflex needs to check for unplugged cpu
             * catch qflex defined number
             */
            r = 0x3F00D; // Random assigned number for now
        }

        PTH_YIELD;
    }


    /* Does not need atomic_mb_set because a spurious wakeup is okay.  */
    atomic_set(&tcg_current_rr_cpu, NULL);

    if(cpu->exit_request) {
        atomic_mb_set(&cpu->exit_request, 0);
    }
    qemu_tcg_wait_io_event(cpu);

    return r;
}

#endif /* CONFIG_FLEXUS */ /* CONFIG_FA_QFLEX */


/* Destroy any remaining vCPUs which have been unplugged and have
 * finished running
 */
static void deal_with_unplugged_cpus(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (cpu->unplug && !cpu_can_run(cpu)) {
            qemu_tcg_destroy_vcpu(cpu);
            cpu->created = false;
            qemu_cond_signal(&qemu_cpu_cond);
        }
    }
}

/* Single-threaded TCG
 *
 * In the single-threaded case each vCPU is simulated in turn. If
 * there is more than a single vCPU we create a simple timer to kick
 * the vCPU and ensure we don't get stuck in a tight loop in one vCPU.
 * This is done explicitly rather than relying on side-effects
 * elsewhere.
 */

static void *qemu_tcg_rr_cpu_thread_fn(void *arg)
{
    PTH_UPDATE_CONTEXT

    CPUState *cpu = arg;

    rcu_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    CPU_FOREACH(cpu) {
        cpu->thread_id = qemu_get_thread_id();
        cpu->created = true;
        cpu->can_do_io = 1;
    }
    qemu_cond_signal(&qemu_cpu_cond);

    /* wait for initial kick-off after machine start */
    while (first_cpu->stopped) {
        qemu_cond_wait(first_cpu->halt_cond, &qemu_global_mutex);

        /* process any pending work */
        CPU_FOREACH(cpu) {
            PTH(current_cpu) = cpu;
            qemu_wait_io_event_common(cpu);
        }
    }

    start_tcg_kick_timer();

    cpu = first_cpu;

    /* process any pending work */
    cpu->exit_request = 1;

#ifdef CONFIG_FLEXUS
    int counter = 0, init_counter = 0;
    bool initialize[64] = {false}, completed[64] = {false}, ff = unlikely(qflex_loglevel_mask(QFLEX_LOG_FF));

    if (!ff) {
        if (flexus_state.mode == TIMING) {
            goto _label_start_timing;
        }
        else if (!qflex_trace_enabled && flexus_state.mode == TRACE) {
            qflex_trace_enabled = true;
            qflex_log_mask(QFLEX_LOG_GENERAL, "QFLEX: TRACE START\n"
                    "    -> Starting trace simulation. Enabling callbacks into Flexus.\n");
        }
    }

#endif
    while (1) {
        /* Account partial waits to QEMU_CLOCK_VIRTUAL.  */
        qemu_account_warp_timer();

        /* Run the timers here.  This is much more efficient than
         * waking up the I/O thread and waiting for completion.
         */
        handle_icount_deadline();

        if (!cpu) {
            cpu = first_cpu;
        }

        while (cpu && !cpu->queued_work_first && !cpu->exit_request) {

            atomic_mb_set(&tcg_current_rr_cpu, cpu);
            PTH(current_cpu) = cpu;

            qemu_clock_enable(QEMU_CLOCK_VIRTUAL,
                              (cpu->singlestep_enabled & SSTEP_NOTIMER) == 0);

            int r = 0;
            if (cpu_can_run(cpu)) {

                prepare_icount_for_run(cpu);

                r = tcg_cpu_exec(cpu);

                process_icount_data(cpu);

#ifdef CONFIG_QUANTUM
                if ( quantum_state.quantum_value > 0 ) {
                    if(cpu->hasReachedInstrLimit){
                        cpu->hasReachedInstrLimit = false;
                    }
                }
                // for debugging purposes
                if (r == EXCP_INTERRUPT || r == EXCP_HLT || r == EXCP_DEBUG
                        || r == EXCP_HALTED || r == EXCP_YIELD || r == EXCP_ATOMIC) {
                    cpu->nr_exp[r-EXCP_INTERRUPT]++;
                }
#endif /* CONFIG_QUANTUM */
                if (r == EXCP_DEBUG) {
                    cpu_handle_guest_debug(cpu);
                    break;
                } else if (r == EXCP_ATOMIC) {
                    qemu_mutex_unlock_iothread();
                    cpu_exec_step_atomic(cpu);
                    qemu_mutex_lock_iothread();
                    break;
                }
            } else if (cpu->stop) {
                if (cpu->unplug) {
                    cpu = CPU_NEXT(cpu);
                }
                break;
            }

#ifdef CONFIG_FLEXUS
            if (ff) {
                if(init_counter == smp_cpus && (ARM_CPU(cpu)->env.pc >> 32) == 0 && !completed[cpu->cpu_index]){
                    counter = 0;
                    CPUState *bkp_cpu = first_cpu;
                    CPU_FOREACH(bkp_cpu) {
                        completed[bkp_cpu->cpu_index] = false;
                        if((ARM_CPU(bkp_cpu)->env.pc >> 32) == 0){
                            qflex_log_mask(QFLEX_LOG_GENERAL, "QFLEX: Core #%u completed FF step.\n",bkp_cpu->cpu_index);
                            completed[bkp_cpu->cpu_index] = true;
                            counter++;
                        }
                    }
                    if(counter == smp_cpus) {
                        // only break for timing, just enable callbacks in trace
                        if (flexus_state.mode == TIMING)
                            break;
                        else if (!qflex_trace_enabled && flexus_state.mode == TRACE) {
                            qflex_trace_enabled = true;
                            qflex_log_mask(QFLEX_LOG_GENERAL, "QFLEX: TRACE START\n"
                                            "    -> Starting trace simulation. Enabling callbacks into Flexus.\n");
                        }
                    }
                } else if(init_counter < smp_cpus && !initialize[cpu->cpu_index]){
                    initialize[cpu->cpu_index] = true;
                    init_counter += 1;
                }
            } 
#endif
            cpu = CPU_NEXT(cpu);

            PTH_YIELD

        } /* while (cpu && !cpu->exit_request).. */

        /* Does not need atomic_mb_set because a spurious wakeup is okay.  */
        atomic_set(&tcg_current_rr_cpu, NULL);

        if (cpu && cpu->exit_request) {
            atomic_mb_set(&cpu->exit_request, 0);
        }
        qemu_tcg_wait_io_event(cpu ? cpu : QTAILQ_FIRST(&cpus));
        deal_with_unplugged_cpus();
#ifdef CONFIG_FLEXUS
        if ((flexus_state.mode == TIMING) && (counter == smp_cpus)) {
            qflex_log_mask(QFLEX_LOG_GENERAL, "QFLEX: Cores successfully fast-forwarded to User mode\n");
            goto _label_start_timing;
        }
#endif
    }
#ifdef CONFIG_FLEXUS
    if (flexus_state.mode == TIMING){
_label_start_timing:
        qflex_log_mask(QFLEX_LOG_GENERAL, "QFLEX: TIMING START\n"
                                          "    -> Starting timing simulation. Passing control to Flexus.\n");
        qflex_control_with_flexus = true;
        startFlexus();
    }
    qflex_log_mask(QFLEX_LOG_GENERAL, "QFLEX: Went outside QEMU and Flexus loops\n");
    CPU_FOREACH(cpu) {
        cpu->stop = true;
        cpu->unplug = true;
    }
    deal_with_unplugged_cpus();
    qemu_mutex_unlock_iothread();
#endif /* CONFIG_FLEXUS */

    return NULL;
}

static void *qemu_hax_cpu_thread_fn(void *arg)
{
    PTH_UPDATE_CONTEXT
    CPUState *cpu = arg;
    int r;

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->created = true;
    cpu->halted = 0;
    PTH(current_cpu) = cpu;
    hax_init_vcpu(cpu);
    qemu_cond_signal(&qemu_cpu_cond);

    while (1) {
        if (cpu_can_run(cpu)) {
            r = hax_smp_cpu_exec(cpu);
            if (r == EXCP_DEBUG) {
                cpu_handle_guest_debug(cpu);
            }
        }

        while (cpu_thread_is_idle(cpu)) {
            qemu_cond_wait(cpu->halt_cond, &qemu_global_mutex);
        }
#ifdef _WIN32
        SleepEx(0, TRUE);
#endif
        qemu_wait_io_event_common(cpu);
    }
    return NULL;
}

#ifdef _WIN32
static void CALLBACK dummy_apc_func(ULONG_PTR unused)
{
}
#endif

/* Multi-threaded TCG
 *
 * In the multi-threaded case each vCPU has its own thread. The TLS
 * variable current_cpu can be used deep in the code to find the
 * current CPUState for a given thread.
 */

static void *qemu_tcg_cpu_thread_fn(void *arg)
{
    PTH_UPDATE_CONTEXT
    CPUState *cpu = arg;

    g_assert(!use_icount);

    rcu_register_thread();

    qemu_mutex_lock_iothread();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->created = true;
    cpu->can_do_io = 1;
#ifdef CONFIG_QUANTUM
    cpu->nr_instr = 0;
    cpu->hasReachedInstrLimit = false;
    cpu->nr_total_instr = 0;
    cpu->nr_quantumHits = 0;
    cpu->nr_exp[0] = 0;
    cpu->nr_exp[1] = 0;
    cpu->nr_exp[2] = 0;
    cpu->nr_exp[3] = 0;
    cpu->nr_exp[4] = 0;
    cpu->nr_exp[5] = 0;
#endif /* CONFIG_QUANTUM */
    PTH(current_cpu) = cpu;
    qemu_cond_signal(&qemu_cpu_cond);

    /* process any pending work */
    cpu->exit_request = 1;

    while (1) {
        if (cpu_can_run(cpu)) {
            int r;
            r = tcg_cpu_exec(cpu);
            switch (r) {
            case EXCP_DEBUG:
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:
                /* during start-up the vCPU is reset and the thread is
                 * kicked several times. If we don't ensure we go back
                 * to sleep in the halted state we won't cleanly
                 * start-up when the vCPU is enabled.
                 *
                 * cpu->halted should ensure we sleep in wait_io_event
                 */
                g_assert(cpu->halted);
                break;
            case EXCP_ATOMIC:
                qemu_mutex_unlock_iothread();
                cpu_exec_step_atomic(cpu);
                qemu_mutex_lock_iothread();
            default:
                /* Ignore everything else? */
                break;
            }
        } else if (cpu->unplug) {
            qemu_tcg_destroy_vcpu(cpu);
            cpu->created = false;
            qemu_cond_signal(&qemu_cpu_cond);
            qemu_mutex_unlock_iothread();
            return NULL;
        }

        atomic_mb_set(&cpu->exit_request, 0);
        qemu_tcg_wait_io_event(cpu);
    }

    return NULL;
}

static void qemu_cpu_kick_thread(CPUState *cpu)
{
#ifndef _WIN32
    int err;

    if (cpu->thread_kicked) {
        return;
    }
    cpu->thread_kicked = true;
#ifndef CONFIG_PTH
    err = pthread_kill(cpu->thread->thread, SIG_IPI);
#else
    err = pthpthread_kill(cpu->thread->wrapper.pth_thread, SIG_IPI);
#endif
    if (err) {
        fprintf(stderr, "qemu:%s: %s", __func__, strerror(err));
        exit(1);
    }
#else /* _WIN32 */
    if (!qemu_cpu_is_self(cpu)) {
        if (!QueueUserAPC(dummy_apc_func, cpu->hThread, 0)) {
            fprintf(stderr, "%s: QueueUserAPC failed with error %lu\n",
                    __func__, GetLastError());
            exit(1);
        }
    }
#endif
}

void qemu_cpu_kick(CPUState *cpu)
{
    qemu_cond_broadcast(cpu->halt_cond);
    if (tcg_enabled()) {
        cpu_exit(cpu);
        /* NOP unless doing single-thread RR */
        qemu_cpu_kick_rr_cpu();
    } else {
        if (hax_enabled()) {
            /*
             * FIXME: race condition with the exit_request check in
             * hax_vcpu_hax_exec
             */
            cpu->exit_request = 1;
        }
        qemu_cpu_kick_thread(cpu);
    }
}

void qemu_cpu_kick_self(void)
{
    PTH_UPDATE_CONTEXT
    assert(PTH(current_cpu));
    qemu_cpu_kick_thread(PTH(current_cpu));
}

bool qemu_cpu_is_self(CPUState *cpu)
{
    return qemu_thread_is_self(cpu->thread);
}

bool qemu_in_vcpu_thread(void)
{
    PTH_UPDATE_CONTEXT
    return PTH(current_cpu) && qemu_cpu_is_self(PTH(current_cpu));
}
#ifndef CONFIG_PTH
static __thread bool iothread_locked = false;
#endif
bool qemu_mutex_iothread_locked(void)
{
    PTH_UPDATE_CONTEXT
    return PTH(iothread_locked);
}

void qemu_mutex_lock_iothread(void)
{
    PTH_UPDATE_CONTEXT
    g_assert(!qemu_mutex_iothread_locked());
    qemu_mutex_lock(&qemu_global_mutex);
    PTH(iothread_locked) = true;
}

void qemu_mutex_unlock_iothread(void)
{
    PTH_UPDATE_CONTEXT
    g_assert(qemu_mutex_iothread_locked());
    PTH(iothread_locked) = false;
    qemu_mutex_unlock(&qemu_global_mutex);
}

static bool all_vcpus_paused(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (!cpu->stopped) {
            return false;
        }
    }

    return true;
}

void pause_all_vcpus(void)
{
    CPUState *cpu;

    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, false);
    CPU_FOREACH(cpu) {
        cpu->stop = true;
        qemu_cpu_kick(cpu);
    }

    if (qemu_in_vcpu_thread()) {
        cpu_stop_current();
    }

    while (!all_vcpus_paused()) {
        qemu_cond_wait(&qemu_pause_cond, &qemu_global_mutex);
        CPU_FOREACH(cpu) {
            qemu_cpu_kick(cpu);
        }
    }
}

void cpu_resume(CPUState *cpu)
{
    cpu->stop = false;
    cpu->stopped = false;
    qemu_cpu_kick(cpu);
}

void resume_all_vcpus(void)
{
    CPUState *cpu;

    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    CPU_FOREACH(cpu) {
        cpu_resume(cpu);
    }
}

void cpu_remove(CPUState *cpu)
{
    cpu->stop = true;
    cpu->unplug = true;
    qemu_cpu_kick(cpu);
}

void cpu_remove_sync(CPUState *cpu)
{
    cpu_remove(cpu);
    while (cpu->created) {
        qemu_cond_wait(&qemu_cpu_cond, &qemu_global_mutex);
    }
}

/* For temporary buffers for forming a name */
#define VCPU_THREAD_NAME_SIZE 16

static void qemu_tcg_init_vcpu(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];
    static QemuCond *single_tcg_halt_cond;
    static QemuThread *single_tcg_cpu_thread;

    if (qemu_tcg_mttcg_enabled() || !single_tcg_cpu_thread) {
        cpu->thread = g_malloc0(sizeof(QemuThread));
        cpu->halt_cond = g_malloc0(sizeof(QemuCond));
        qemu_cond_init(cpu->halt_cond);

        if (qemu_tcg_mttcg_enabled()) {
            /* create a thread per vCPU with TCG (MTTCG) */
            parallel_cpus = true;
            snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/TCG",
                 cpu->cpu_index);

            qemu_thread_create(cpu->thread, thread_name, qemu_tcg_cpu_thread_fn,
                               cpu, QEMU_THREAD_JOINABLE);

        } else {
            /* share a single thread for all cpus with TCG */
            snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "ALL CPUs/TCG");
            qemu_thread_create(cpu->thread, thread_name,
                               qemu_tcg_rr_cpu_thread_fn,
                               cpu, QEMU_THREAD_JOINABLE);

            single_tcg_halt_cond = cpu->halt_cond;
            single_tcg_cpu_thread = cpu->thread;
        }
#ifdef _WIN32
        cpu->hThread = qemu_thread_get_handle(cpu->thread);
#endif
        while (!cpu->created) {
            qemu_cond_wait(&qemu_cpu_cond, &qemu_global_mutex);
        }
    } else {
        /* For non-MTTCG cases we share the thread */
        cpu->thread = single_tcg_cpu_thread;
        cpu->halt_cond = single_tcg_halt_cond;
    }
}

static void qemu_hax_start_vcpu(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    cpu->thread = g_malloc0(sizeof(QemuThread));
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);

    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/HAX",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, qemu_hax_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
#ifdef _WIN32
    cpu->hThread = qemu_thread_get_handle(cpu->thread);
#endif
    while (!cpu->created) {
        qemu_cond_wait(&qemu_cpu_cond, &qemu_global_mutex);
    }
}

static void qemu_kvm_start_vcpu(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    cpu->thread = g_malloc0(sizeof(QemuThread));
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/KVM",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, qemu_kvm_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
    while (!cpu->created) {
        qemu_cond_wait(&qemu_cpu_cond, &qemu_global_mutex);
    }
}

static void qemu_dummy_start_vcpu(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    cpu->thread = g_malloc0(sizeof(QemuThread));
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/DUMMY",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, qemu_dummy_cpu_thread_fn, cpu,
                       QEMU_THREAD_JOINABLE);
    while (!cpu->created) {
        qemu_cond_wait(&qemu_cpu_cond, &qemu_global_mutex);
    }
}

void qemu_init_vcpu(CPUState *cpu)
{
    cpu->nr_cores = smp_cores;
    cpu->nr_threads = smp_threads;
    cpu->stopped = true;

    if (!cpu->as) {
        /* If the target cpu hasn't set up any address spaces itself,
         * give it the default one.
         */
        AddressSpace *as = g_new0(AddressSpace, 1);

        address_space_init(as, cpu->memory, "cpu-memory");
        cpu->num_ases = 1;
        cpu_address_space_init(cpu, as, 0);
    }

    if (kvm_enabled()) {
        qemu_kvm_start_vcpu(cpu);
    } else if (hax_enabled()) {
        qemu_hax_start_vcpu(cpu);
    } else if (tcg_enabled()) {
        qemu_tcg_init_vcpu(cpu);
    } else {
        qemu_dummy_start_vcpu(cpu);
    }
}

void cpu_stop_current(void)
{
    PTH_UPDATE_CONTEXT
    if (PTH(current_cpu)) {
        PTH(current_cpu)->stop = false;
        PTH(current_cpu)->stopped = true;
        cpu_exit(PTH(current_cpu));
        qemu_cond_broadcast(&qemu_pause_cond);
    }
}

int vm_stop(RunState state)
{
    if (qemu_in_vcpu_thread()) {
        qemu_system_vmstop_request_prepare();
        qemu_system_vmstop_request(state);
        /*
         * FIXME: should not return to device code in case
         * vm_stop() has been requested.
         */
        cpu_stop_current();
        return 0;
    }

    return do_vm_stop(state);
}

/**
 * Prepare for (re)starting the VM.
 * Returns -1 if the vCPUs are not to be restarted (e.g. if they are already
 * running or in case of an error condition), 0 otherwise.
 */
int vm_prepare_start(void)
{
    RunState requested;
    int res = 0;

    qemu_vmstop_requested(&requested);
    if (runstate_is_running() && requested == RUN_STATE__MAX) {
        return -1;
    }

    /* Ensure that a STOP/RESUME pair of events is emitted if a
     * vmstop request was pending.  The BLOCK_IO_ERROR event, for
     * example, according to documentation is always followed by
     * the STOP event.
     */
    if (runstate_is_running()) {
        qapi_event_send_stop(&error_abort);
        res = -1;
    } else {
        replay_enable_events();
        cpu_enable_ticks();
        runstate_set(RUN_STATE_RUNNING);
        vm_state_notify(1, RUN_STATE_RUNNING);
    }

    /* We are sending this now, but the CPUs will be resumed shortly later */
    qapi_event_send_resume(&error_abort);
    return res;
}

void vm_start(void)
{
    if (!vm_prepare_start()) {
        resume_all_vcpus();
    }
}

/* does a state transition even if the VM is already stopped,
   current state is forgotten forever */
int vm_stop_force_state(RunState state)
{
    if (runstate_is_running()) {
        return vm_stop(state);
    } else {
        runstate_set(state);

        bdrv_drain_all();
        /* Make sure to return an error if the flush in a previous vm_stop()
         * failed. */
        return bdrv_flush_all();
    }
}

void list_cpus(FILE *f, fprintf_function cpu_fprintf, const char *optarg)
{
    /* XXX: implement xxx_cpu_list for targets that still miss it */
#if defined(cpu_list)
    cpu_list(f, cpu_fprintf);
#endif
}

CpuInfoList *qmp_query_cpus(Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    CpuInfoList *head = NULL, *cur_item = NULL;
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        CpuInfoList *info;
#if defined(TARGET_I386)
        X86CPU *x86_cpu = X86_CPU(cpu);
        CPUX86State *env = &x86_cpu->env;
#elif defined(TARGET_PPC)
        PowerPCCPU *ppc_cpu = POWERPC_CPU(cpu);
        CPUPPCState *env = &ppc_cpu->env;
#elif defined(TARGET_SPARC)
        SPARCCPU *sparc_cpu = SPARC_CPU(cpu);
        CPUSPARCState *env = &sparc_cpu->env;
#elif defined(TARGET_MIPS)
        MIPSCPU *mips_cpu = MIPS_CPU(cpu);
        CPUMIPSState *env = &mips_cpu->env;
#elif defined(TARGET_TRICORE)
        TriCoreCPU *tricore_cpu = TRICORE_CPU(cpu);
        CPUTriCoreState *env = &tricore_cpu->env;
#endif

        cpu_synchronize_state(cpu);

        info = g_malloc0(sizeof(*info));
        info->value = g_malloc0(sizeof(*info->value));
        info->value->CPU = cpu->cpu_index;
        info->value->current = (cpu == first_cpu);
        info->value->halted = cpu->halted;
        info->value->qom_path = object_get_canonical_path(OBJECT(cpu));
        info->value->thread_id = cpu->thread_id;
#if defined(TARGET_I386)
        info->value->arch = CPU_INFO_ARCH_X86;
        info->value->u.x86.pc = env->eip + env->segs[R_CS].base;
#elif defined(TARGET_PPC)
        info->value->arch = CPU_INFO_ARCH_PPC;
        info->value->u.ppc.nip = env->nip;
#elif defined(TARGET_SPARC)
        info->value->arch = CPU_INFO_ARCH_SPARC;
        info->value->u.q_sparc.pc = env->pc;
        info->value->u.q_sparc.npc = env->npc;
#elif defined(TARGET_MIPS)
        info->value->arch = CPU_INFO_ARCH_MIPS;
        info->value->u.q_mips.PC = env->active_tc.PC;
#elif defined(TARGET_TRICORE)
        info->value->arch = CPU_INFO_ARCH_TRICORE;
        info->value->u.tricore.PC = env->PC;
#else
        info->value->arch = CPU_INFO_ARCH_OTHER;
#endif
        info->value->has_props = !!mc->cpu_index_to_instance_props;
        if (info->value->has_props) {
            CpuInstanceProperties *props;
            props = g_malloc0(sizeof(*props));
            *props = mc->cpu_index_to_instance_props(ms, cpu->cpu_index);
            info->value->props = props;
        }

        /* XXX: waiting for the qapi to support GSList */
        if (!cur_item) {
            head = cur_item = info;
        } else {
            cur_item->next = info;
            cur_item = info;
        }
    }

    return head;
}

void qmp_memsave(int64_t addr, int64_t size, const char *filename,
                 bool has_cpu, int64_t cpu_index, Error **errp)
{
    FILE *f;
    uint32_t l;
    CPUState *cpu;
    uint8_t buf[1024];
    int64_t orig_addr = addr, orig_size = size;

    if (!has_cpu) {
        cpu_index = 0;
    }

    cpu = qemu_get_cpu(cpu_index);
    if (cpu == NULL) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "cpu-index",
                   "a CPU number");
        return;
    }

    f = fopen(filename, "wb");
    if (!f) {
        error_setg_file_open(errp, errno, filename);
        return;
    }

    while (size != 0) {
        l = sizeof(buf);
        if (l > size)
            l = size;
        if (cpu_memory_rw_debug(cpu, addr, buf, l, 0) != 0) {
            error_setg(errp, "Invalid addr 0x%016" PRIx64 "/size %" PRId64
                             " specified", orig_addr, orig_size);
            goto exit;
        }
        if (fwrite(buf, 1, l, f) != l) {
            error_setg(errp, QERR_IO_ERROR);
            goto exit;
        }
        addr += l;
        size -= l;
    }

exit:
    fclose(f);
}

void qmp_pmemsave(int64_t addr, int64_t size, const char *filename,
                  Error **errp)
{
    FILE *f;
    uint32_t l;
    uint8_t buf[1024];

    f = fopen(filename, "wb");
    if (!f) {
        error_setg_file_open(errp, errno, filename);
        return;
    }

    while (size != 0) {
        l = sizeof(buf);
        if (l > size)
            l = size;
        cpu_physical_memory_read(addr, buf, l);
        if (fwrite(buf, 1, l, f) != l) {
            error_setg(errp, QERR_IO_ERROR);
            goto exit;
        }
        addr += l;
        size -= l;
    }

exit:
    fclose(f);
}

void qmp_inject_nmi(Error **errp)
{
    nmi_monitor_handle(monitor_get_cpu_index(), errp);
}

void dump_drift_info(FILE *f, fprintf_function cpu_fprintf)
{
    if (!use_icount) {
        return;
    }

    cpu_fprintf(f, "Host - Guest clock  %"PRIi64" ms\n",
                (cpu_get_clock() - cpu_get_icount())/SCALE_MS);
    if (icount_align_option) {
        cpu_fprintf(f, "Max guest delay     %"PRIi64" ms\n", -max_delay/SCALE_MS);
        cpu_fprintf(f, "Max guest advance   %"PRIi64" ms\n", max_advance/SCALE_MS);
    } else {
        cpu_fprintf(f, "Max guest delay     NA\n");
        cpu_fprintf(f, "Max guest advance   NA\n");
    }
}
#ifdef CONFIG_FLEXUS
//NOOSHIN: test begin
int get_info(void *opaque){
    CPUState* cpu = (CPUState*)opaque;

    int r = 0;
    printf("QEMU: in get_info\n");

    /* Account partial waits to QEMU_CLOCK_VIRTUAL.  */
    qemu_account_warp_timer();

    qemu_clock_enable(QEMU_CLOCK_VIRTUAL,
                      (cpu->singlestep_enabled & SSTEP_NOTIMER) == 0);

    if (cpu_can_run(cpu)) {
        r = tcg_cpu_exec(cpu);
        if (r == EXCP_DEBUG) {
            cpu_handle_guest_debug(cpu);

        }
    }
    /* Pairs with smp_wmb in qemu_cpu_kick.  */
    // atomic_mb_set(&exit_request, 0);

    if (use_icount) {
        int64_t deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL);

        if (deadline == 0) {
            qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
        }
    }
    qemu_tcg_wait_io_event(QTAILQ_FIRST(&cpus));

    return 0;
}
//NOOSHIN: test end
#endif /* CONFIG_FLEXUS */
