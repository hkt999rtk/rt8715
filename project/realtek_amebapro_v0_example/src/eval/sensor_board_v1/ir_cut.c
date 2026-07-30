/**
 * The IR cut only use one GPIO
 *     1: enable IR cut
 *     0: disable IR cut
 *
 * In normal case, the IR cut is always enabled.
 * In low light situation, IR led is turned on and IR cut should be disabled.
 */

#include "platform_opts.h"

#include "sensor_service.h"
#include "ir_cut.h"

#include "gpio_api.h"

#if 0
static gpio_t gpio_ir_cut;
#endif

int ir_cut_init(void *param)
{
    #if 0
    gpio_init(&gpio_ir_cut, GPIO_IR_CUT_PIN);
    gpio_dir(&gpio_ir_cut, PIN_OUTPUT);
    gpio_mode(&gpio_ir_cut, PullNone);
    gpio_write(&gpio_ir_cut, 1);
    #endif
	
    return 0;
}

int ir_cut_enable(int enable)
{
	#if 0
    gpio_write(&gpio_ir_cut, enable);
	#endif
    return 0;
}