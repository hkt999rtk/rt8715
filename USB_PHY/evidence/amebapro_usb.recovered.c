/*
 * Behavioral reconstruction of lib_usbsmart.a(amebapro_usb.o).
 * Source: ARM Thumb disassembly of member SHA-256
 * 9f929c44bf77543ffc521b3cf0c14cd0aa7ba3cbf031230a78c2bc941e86e182.
 * This is an analysis artifact, not a drop-in replacement for the vendor object.
 * Register names are inferred from accessed addresses; the binary does not
 * carry the original C types, field names, or source-level error handling.
 */
#include <stdint.h>
#include <stdio.h>

extern void hal_delay_us(uint32_t us);
extern void hal_delay_ms(uint32_t ms);

#define MMIO8(address)  (*(volatile uint8_t *)(uintptr_t)(address))
#define MMIO16(address) (*(volatile uint16_t *)(uintptr_t)(address))
#define MMIO32(address) (*(volatile uint32_t *)(uintptr_t)(address))

#define USB_CTRL        0x400F0004u
#define USB_PHY_DATA    0x400F001Cu
#define USB_SYSCON      0x40000244u
#define DWC_GPVNDCTL    0x400C0034u
#define VENDOR_BUSY     (1u << 26)
#define VENDOR_DONE     (1u << 27)

/* The object waits without a software timeout in its write path. */
static void DWCWritePhyReg(uint8_t address, uint8_t value)
{
	while (MMIO32(DWC_GPVNDCTL) & VENDOR_BUSY) {}
	MMIO32(USB_PHY_DATA) = value;
	MMIO32(DWC_GPVNDCTL) = 0x0A300000u | ((address & 0x0Fu) << 8);
	while (!(MMIO32(DWC_GPVNDCTL) & VENDOR_DONE)) {}
	MMIO32(DWC_GPVNDCTL) = 0x0A300000u | ((address >> 4) << 8);
	while (!(MMIO32(DWC_GPVNDCTL) & VENDOR_DONE)) {}
}

/* The compiler specialized the original read routine for address D4. */
static uint8_t DWCReadPhyReg_D4(void)
{
	uint32_t attempts = 0x000FFFFFu;

	printf("[USB] DWCReadPhyReg enter addr=0xd4\r\n");
	while ((MMIO32(DWC_GPVNDCTL) & VENDOR_BUSY) && --attempts) {}
	if (!attempts)
		return 0xFFu;
	MMIO32(DWC_GPVNDCTL) = 0x0A300400u;
	attempts = 0x000FFFFFu;
	while (!(MMIO32(DWC_GPVNDCTL) & VENDOR_DONE) && --attempts) {}
	if (!attempts)
		return 0xFFu;
	MMIO32(DWC_GPVNDCTL) = 0x0A300D00u;
	attempts = 0x000FFFFFu;
	while (!(MMIO32(DWC_GPVNDCTL) & VENDOR_DONE) && --attempts) {}
	if (!attempts)
		return 0xFFu;
	uint8_t value = (uint8_t)MMIO32(DWC_GPVNDCTL);
	printf("[USB] DWCReadPhyReg exit val=0x%02x\r\n", value);
	return value;
}

static const void *usb_chip_get_cal_data(uint8_t mode)
{
	(void)mode;
	return 0;
}

static int usb_chip_init(void)
{
	MMIO8(USB_SYSCON + 2u) |= 0x08u;
	hal_delay_us(500);
	MMIO8(USB_SYSCON + 2u) &= (uint8_t)~0x10u;
	hal_delay_us(1000);
	MMIO8(USB_SYSCON + 2u) |= 0x04u;
	hal_delay_us(100);
	MMIO8(USB_SYSCON + 2u) |= 0x21u;
	MMIO8(USB_SYSCON + 3u) &= (uint8_t)~0x03u;
	hal_delay_us(1);
	MMIO8(USB_SYSCON) |= 0x11u;
	hal_delay_us(200);
	MMIO16(USB_CTRL) |= 0x0200u;
	hal_delay_us(200);
	while (!(MMIO32(USB_CTRL) & (1u << 5))) {}
	MMIO16(USB_CTRL) |= 0x0100u;

	/* F4[6:5] selects the PHY page.  The base object writes page 0 and 1. */
	DWCWritePhyReg(0xF4u, DWCReadPhyReg_D4() & 0x9Fu);
	DWCWritePhyReg(0xE0u, 0x6Cu);
	DWCWritePhyReg(0xE1u, 0x81u);
	DWCWritePhyReg(0xE2u, 0x62u);
	DWCWritePhyReg(0xE7u, 0x41u);
	DWCWritePhyReg(0xF4u, (DWCReadPhyReg_D4() & 0x9Fu) | 0x20u);
	DWCWritePhyReg(0xE0u, 0x91u);
	DWCWritePhyReg(0xF4u, DWCReadPhyReg_D4() & 0x9Fu);
	return 0;
}

static int usb_chip_deinit(void)
{
	MMIO16(USB_CTRL) &= (uint16_t)~0x0300u;
	hal_delay_ms(5);
	MMIO8(USB_SYSCON) &= 0xEEu;
	MMIO8(USB_SYSCON + 3u) |= 0x03u;
	MMIO8(USB_SYSCON + 2u) =
		(uint8_t)((MMIO8(USB_SYSCON + 2u) & 0xD2u) | 0x10u);
	return 0;
}

/* Offsets 0/4/8 in the original 12-byte usb_hal_driver object. */
struct recovered_usb_hal_driver {
	int (*chip_init)(void);
	int (*chip_deinit)(void);
	const void *(*get_cal_data)(uint8_t mode);
};

static const struct recovered_usb_hal_driver reconstructed_driver = {
	usb_chip_init, usb_chip_deinit, usb_chip_get_cal_data
};

/* The binary also has usb_otg_select_mode() as a no-op and
 * usb_otg_uphy_init() returning zero.  Neither adds PHY register writes.
 */
