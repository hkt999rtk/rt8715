#include "platform_opts.h"

#include "sensor_service.h"
#include "sensor_external_ctrl.h"

#include "isp_api.h"
#include "isp_cmd_api.h"

int sensor_external_set_gray_mode(int enable)
{
    if (isp_stream_get_status(0) != 0) {
		if(enable)
		{
			isp_set_gray_mode(enable);
			isp_set_IR_mode(enable);
		}
		else
		{
			isp_set_IR_mode(enable);
			isp_set_gray_mode(enable);
		}
        return 0;
    } else {
        return -1;
    }
}

int sensor_external_loop()
{
    /* there is already a delay at ambient light sensor, so leave this implementation as empty */
    return 0;
}