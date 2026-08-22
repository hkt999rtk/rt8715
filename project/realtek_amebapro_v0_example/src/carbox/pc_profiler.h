#ifndef CARBOX_PC_PROFILER_H
#define CARBOX_PC_PROFILER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the low-overhead statistical PC profiler.  The implementation creates
 * its reporting task here, but reserves and starts the GTimer only after the
 * scheduler is running.
 */
void carbox_pc_profiler_start(void);

/* Monotonic accepted PC-sample counters used to correlate short blocking
 * windows with actual IDLE or gcd-work execution. */
typedef struct carbox_pcprof_task_samples_s {
	unsigned total;
	unsigned idle;
	unsigned gcd_work;
} carbox_pcprof_task_samples_t;

void carbox_pc_profiler_task_samples_snapshot(
	carbox_pcprof_task_samples_t *snapshot);

/* Save the early PLL/SPIC result before console output can be observed. */
void carbox_pc_profiler_set_clock_boot_status(int status);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_PC_PROFILER_H */
