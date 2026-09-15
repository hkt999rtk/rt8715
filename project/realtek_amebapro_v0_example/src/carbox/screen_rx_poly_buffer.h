#ifndef CARBOX_SCREEN_RX_POLY_BUFFER_H
#define CARBOX_SCREEN_RX_POLY_BUFFER_H

#include <stddef.h>
#include <stdint.h>

/* These helpers do not transfer ownership. The existing handover references
 * still decide when the final release is allowed. Unknown pointers use free. */
void *carbox_screen_rx_poly_alloc(size_t wire_length);
void carbox_screen_rx_poly_free(void *payload);

/* Only the allocating RX task, exact ciphertext span and 128-byte screen AAD
 * can borrow the prefix. No probing before an unknown pointer is permitted.
 * The caller must save/restore the wire tag before reusing the tail. */
uint8_t *carbox_screen_rx_poly_input(const void *payload, size_t data_length,
                                    size_t aad_length, size_t input_length);

#endif
