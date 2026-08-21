#ifndef CARBOX_CRYPTO_PRIORITY_LOCK_H
#define CARBOX_CRYPTO_PRIORITY_LOCK_H

#include <stdint.h>

#include "device_lock.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CarPlay AES/ChaCha transaction lock.
 *
 * The RTL8195B has one shared crypto engine.  A priority-4 video task used to
 * keep RT_DEV_LOCK_CRYPTO while it was descheduled behind TCP/IP/WLAN tasks,
 * even after the completion IRQ had fired.  These entry points temporarily
 * raise only a task that is acquiring/owning that engine, then restore its
 * exact original priority after releasing it.
 *
 * Set CARBOX_CRYPTO_OWNER_BOOST_PRIORITY to 0 to compile the policy out.
 */
void carbox_crypto_aes_device_lock(RT_DEV_LOCK_E device);
void carbox_crypto_aes_device_unlock(RT_DEV_LOCK_E device);
void carbox_crypto_chacha_device_lock(RT_DEV_LOCK_E device);
void carbox_crypto_chacha_device_unlock(RT_DEV_LOCK_E device);

/*
 * The RTL crypto adapter owns one global completion callback set, shared by
 * AES, ChaCha and Poly1305.  Keep one controller installed for every
 * algorithm instead of switching callback/semaphore ownership per request.
 * The caller must hold RT_DEV_LOCK_CRYPTO while enabling or resetting it.
 */
int carbox_crypto_irq_controller_enable(void);
void carbox_crypto_irq_controller_vendor_enable(
	void *adapter, void (*ignored_handler)(int, int)
);
void carbox_crypto_irq_controller_engine_reset(void);
int carbox_crypto_irq_controller_last_timed_out(void);
void carbox_crypto_irq_controller_report(unsigned window_index);

typedef struct carbox_crypto_irq_snapshot_s {
	uint32_t timeout_count;
	uint32_t reset_count;
	uint32_t generation;
	uint32_t last_timeout_at_us;
	uint32_t last_timeout_generation;
	uint32_t last_timeout_kind;
	uintptr_t last_timeout_task;
	uint32_t last_timeout_priority;
} carbox_crypto_irq_snapshot_t;

/* Cheap, read-only context for error-only profilers. */
void carbox_crypto_irq_controller_snapshot(
	carbox_crypto_irq_snapshot_t *snapshot
);

#define CARBOX_CRYPTO_KIND_NONE   0u
#define CARBOX_CRYPTO_KIND_AES    1u
#define CARBOX_CRYPTO_KIND_CHACHA 2u
unsigned carbox_crypto_priority_current_kind(void);

#ifdef __cplusplus
}
#endif

#endif
