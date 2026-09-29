/*
 * Interpose the AmebaPro USB chip driver without modifying lib_usbsmart.a.
 *
 * usb_hal_driver is a three-function table in amebapro_usb.o.  References
 * from usb_hal.o, usbh_hcd.o and usbd_pcd.o are undefined relocations, so
 * GNU ld --wrap=usb_hal_driver redirects them to this table.  The original
 * table remains available through __real_usb_hal_driver.
 *
 * Experimental PRO1 RX boost variants use SYSON HS eFuse SysCfg2[2:1].
 * PRO1 documents the register at 0x40000108 but does not name those two bits;
 * their RX boost interpretation comes from the AmebaSmart SEC map.  The
 * wrapper logs and verifies the register operation for board validation.
 */
#include <stdint.h>
#include <stdio.h>

#ifndef CARBOX_USB_RX_BOOST_LEVEL
#define CARBOX_USB_RX_BOOST_LEVEL -1
#endif

#if CARBOX_USB_RX_BOOST_LEVEL < -1 || CARBOX_USB_RX_BOOST_LEVEL > 3
#error "CARBOX_USB_RX_BOOST_LEVEL must be -1 (disabled) or 0..3"
#endif

#define CARBOX_USB_SYSCFG2_ADDR 0x40000108u
#define CARBOX_USB_RX_BOOST_MASK 0x6u
#define CARBOX_USB_RX_BOOST_SHIFT 1u
#define CARBOX_STRINGIFY_INNER(x) #x
#define CARBOX_STRINGIFY(x) CARBOX_STRINGIFY_INNER(x)

typedef struct {
	int (*chip_init)(void);
	int (*chip_deinit)(void);
	void *(*get_cal_data)(uint8_t mode);
} carbox_usb_hal_driver_t;

extern carbox_usb_hal_driver_t __real_usb_hal_driver;

static int carbox_usb_chip_init(void)
{
	int status;
#if CARBOX_USB_RX_BOOST_LEVEL >= 0
	volatile uint32_t *const syscfg2 =
		(volatile uint32_t *)(uintptr_t)CARBOX_USB_SYSCFG2_ADDR;
	uint32_t before = *syscfg2;
	uint32_t target = (before & ~CARBOX_USB_RX_BOOST_MASK) |
		((uint32_t)CARBOX_USB_RX_BOOST_LEVEL << CARBOX_USB_RX_BOOST_SHIFT);

	/* Apply before the vendor turns the PHY on; preserve all other fields. */
	*syscfg2 = target;
	printf("[USB BOOST] candidate level="
	       CARBOX_STRINGIFY(CARBOX_USB_RX_BOOST_LEVEL)
	       " syscfg2 before=%08lx target=%08lx read=%08lx\r\n",
	       (unsigned long)before, (unsigned long)target,
	       (unsigned long)*syscfg2);
	if ((*syscfg2 & CARBOX_USB_RX_BOOST_MASK) !=
	    (target & CARBOX_USB_RX_BOOST_MASK))
		return -1;
#endif

	/* Power/clock sequencing and the vendor baseline PHY writes. */
	status = __real_usb_hal_driver.chip_init();
#if CARBOX_USB_RX_BOOST_LEVEL >= 0
	if (status == 0) {
		uint32_t after = *syscfg2;
		printf("[USB BOOST] after PHY init=%08lx\r\n",
		       (unsigned long)after);
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
