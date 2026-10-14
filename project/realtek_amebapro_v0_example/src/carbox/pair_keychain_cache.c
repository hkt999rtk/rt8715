#include <stdint.h>

/* The vendor SecItem entry points hold gKeychainLock across these calls. */
extern int carbox_pair_keychain_read_uncached(void);
extern int carbox_pair_keychain_write_uncached(void);
extern void *gKeychainItems;

static uint8_t keychain_loaded;

int carbox_pair_keychain_read_cached(void)
{
	int status;

	if (keychain_loaded && gKeychainItems != 0) {
		return 0;
	}
	status = carbox_pair_keychain_read_uncached();
	if (status == 0 && gKeychainItems != 0) {
		keychain_loaded = 1;
	}
	return status;
}

int carbox_pair_keychain_write_cached(void)
{
	int status = carbox_pair_keychain_write_uncached();

	if (status != 0) {
		/* A failed write must not leave an in-memory-only pairing trusted. */
		keychain_loaded = 0;
	}
	return status;
}
