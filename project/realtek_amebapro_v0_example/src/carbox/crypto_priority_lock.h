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

#define CARBOX_CRYPTO_KIND_NONE   0u
#define CARBOX_CRYPTO_KIND_AES    1u
#define CARBOX_CRYPTO_KIND_CHACHA 2u
unsigned carbox_crypto_priority_current_kind(void);

#ifdef __cplusplus
}
#endif

#endif
