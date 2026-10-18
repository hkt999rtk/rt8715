/*
 * Interpose the AmebaPro USB chip driver without modifying lib_usbsmart.a.
 *
 * usb_hal_driver is a three-function table in amebapro_usb.o.  References
 * from usb_hal.o, usbh_hcd.o and usbd_pcd.o are undefined relocations, so
 * GNU ld --wrap=usb_hal_driver redirects them to this table.  The original
 * table remains available through __real_usb_hal_driver.
 *
 * The RLE0799 (AmebaPro) PHY register table defines page 0 E4[3:2] as
 * REG_RX_BOOST.  Apply the requested level after vendor PHY initialization,
 * which leaves page 0 selected, and verify the PHY register readback.
 */
#include <stdint.h>
#include <stdio.h>

#ifndef CARBOX_USB_RX_BOOST_LEVEL
#define CARBOX_USB_RX_BOOST_LEVEL -1
#endif

#if CARBOX_USB_RX_BOOST_LEVEL < -1 || CARBOX_USB_RX_BOOST_LEVEL > 3
#error "CARBOX_USB_RX_BOOST_LEVEL must be -1 (disabled) or 0..3"
#endif

#define CARBOX_USB_GPVNDCTL_ADDR 0x400C0034u
#define CARBOX_USB_PHY_DATA_ADDR 0x400F001Cu
#define CARBOX_USB_PHY_E4 0xE4u
#define CARBOX_USB_RX_BOOST_MASK 0x0Cu
#define CARBOX_USB_RX_BOOST_SHIFT 2u
#define CARBOX_USB_VND_BUSY (1u << 26)
#define CARBOX_USB_VND_DONE (1u << 27)
#define CARBOX_USB_VND_CMD 0x0A300000u
#define CARBOX_USB_PHY_TIMEOUT 0x000FFFFFu
#define CARBOX_STRINGIFY_INNER(x) #x
#define CARBOX_STRINGIFY(x) CARBOX_STRINGIFY_INNER(x)

typedef struct {
	int (*chip_init)(void);
	int (*chip_deinit)(void);
	void *(*get_cal_data)(uint8_t mode);
} carbox_usb_hal_driver_t;

extern carbox_usb_hal_driver_t __real_usb_hal_driver;

#if CARBOX_USB_RX_BOOST_LEVEL >= 0
static int carbox_usb_wait_vendor(uint32_t mask, int set)
{
	volatile uint32_t *const control =
		(volatile uint32_t *)(uintptr_t)CARBOX_USB_GPVNDCTL_ADDR;
	uint32_t remaining = CARBOX_USB_PHY_TIMEOUT;

	while (remaining--) {
		if (((*control & mask) != 0u) == set)
			return 0;
	}
	return -1;
}

static int carbox_usb_phy_read(uint8_t address, uint8_t *value)
{
	volatile uint32_t *const control =
		(volatile uint32_t *)(uintptr_t)CARBOX_USB_GPVNDCTL_ADDR;

	if (carbox_usb_wait_vendor(CARBOX_USB_VND_BUSY, 0) != 0)
		return -1;
	*control = CARBOX_USB_VND_CMD | ((uint32_t)(address & 0x0Fu) << 8);
	if (carbox_usb_wait_vendor(CARBOX_USB_VND_DONE, 1) != 0)
		return -1;
	*control = CARBOX_USB_VND_CMD | ((uint32_t)(address >> 4) << 8);
	if (carbox_usb_wait_vendor(CARBOX_USB_VND_DONE, 1) != 0)
		return -1;
	*value = (uint8_t)*control;
	return 0;
}

static int carbox_usb_phy_write(uint8_t address, uint8_t value)
{
	volatile uint32_t *const control =
		(volatile uint32_t *)(uintptr_t)CARBOX_USB_GPVNDCTL_ADDR;
	volatile uint32_t *const data =
		(volatile uint32_t *)(uintptr_t)CARBOX_USB_PHY_DATA_ADDR;

	if (carbox_usb_wait_vendor(CARBOX_USB_VND_BUSY, 0) != 0)
		return -1;
	*data = value;
	*control = CARBOX_USB_VND_CMD | ((uint32_t)(address & 0x0Fu) << 8);
	if (carbox_usb_wait_vendor(CARBOX_USB_VND_DONE, 1) != 0)
		return -1;
	*control = CARBOX_USB_VND_CMD | ((uint32_t)(address >> 4) << 8);
	return carbox_usb_wait_vendor(CARBOX_USB_VND_DONE, 1);
}
#endif

static int carbox_usb_chip_init(void)
{
	int status = __real_usb_hal_driver.chip_init();
#if CARBOX_USB_RX_BOOST_LEVEL >= 0
	if (status == 0) {
		uint8_t before, target, after;

		if (carbox_usb_phy_read(CARBOX_USB_PHY_E4, &before) != 0) {
			printf("[USB BOOST] PHY E4 read timeout\r\n");
			return -1;
		}
		target = (uint8_t)((before & ~CARBOX_USB_RX_BOOST_MASK) |
			((uint8_t)CARBOX_USB_RX_BOOST_LEVEL << CARBOX_USB_RX_BOOST_SHIFT));
		if (carbox_usb_phy_write(CARBOX_USB_PHY_E4, target) != 0 ||
		    carbox_usb_phy_read(CARBOX_USB_PHY_E4, &after) != 0) {
			printf("[USB BOOST] PHY E4 write/read timeout\r\n");
			return -1;
		}
		printf("[USB BOOST] PHY p0:E4 level="
		       CARBOX_STRINGIFY(CARBOX_USB_RX_BOOST_LEVEL)
		       " before=%02x target=%02x read=%02x\r\n",
		       (unsigned int)before, (unsigned int)target,
		       (unsigned int)after);
		if ((after & CARBOX_USB_RX_BOOST_MASK) !=
		    (target & CARBOX_USB_RX_BOOST_MASK))
			return -1;
	}
#endif
	return status;
}

static int carbox_usb_chip_deinit(void)
{
	return __real_usb_hal_driver.chip_deinit();
}

static void *carbox_usb_get_cal_data(uint8_t mode)
{
	return __real_usb_hal_driver.get_cal_data(mode);
}

carbox_usb_hal_driver_t __wrap_usb_hal_driver = {
	carbox_usb_chip_init,
	carbox_usb_chip_deinit,
	carbox_usb_get_cal_data
};
