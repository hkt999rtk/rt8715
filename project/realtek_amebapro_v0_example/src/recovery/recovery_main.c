#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "log_service.h"
#include "wifi_conf.h"
#include "wifi_constants.h"
#include "lwip_netconf.h"
#include "ota_8195b.h"
#include "hal_flash_boot.h"
#include "carbox_flash_layout.h"
#include "wlan_fast_connect/example_wlan_fast_connect.h"

#include <string.h>

extern void console_init(void);

/* Recovery never starts ISP, so flash access has no competing ISP owner. */
void isp_mcu_lock(void) {}
void isp_mcu_release(void) {}

/* Minimal replacements for state normally owned by the full Wi-Fi AT app. */
rtw_mode_t wifi_mode = RTW_MODE_STA;
struct static_ip_config user_static_ip;
struct user_ip_config user_ip;
write_reconnect_ptr p_write_reconnect_ptr;
uint32_t offer_ip;
uint32_t server_ip;

void print_wlan_help(void)
{
	rt_printf("Recovery commands: ATRC and ATRO\r\n");
}

static void recovery_wifi(void *arg)
{
	char *separator;
	char *ssid = (char *)arg;
	char *password;
	int ret;

	if (ssid == NULL || (separator = strchr(ssid, ',')) == NULL) {
		rt_printf("Usage: ATRC=SSID,PASSWORD\r\n");
		return;
	}
	*separator = '\0';
	password = separator + 1;

	ret = wifi_connect(ssid, RTW_SECURITY_WPA2_AES_PSK, password,
			   strlen(ssid), strlen(password), -1, NULL);
	if (ret != RTW_SUCCESS) {
		rt_printf("[RECOVERY] Wi-Fi connect failed: %d\r\n", ret);
		return;
	}

	LwIP_DHCP(0, DHCP_START);
	rt_printf("[RECOVERY] Wi-Fi connected; DHCP completed\r\n");
}

static void recovery_update(void *arg)
{
	char *separator;
	char *server = (char *)arg;
	int port;

	if (server == NULL || (separator = strchr(server, ',')) == NULL) {
		rt_printf("Usage: ATRO=SERVER_IP,PORT\r\n");
		return;
	}
	*separator = '\0';
	port = atoi(separator + 1);
	if (port <= 0 || port > 65535) {
		rt_printf("[RECOVERY] invalid OTA port\r\n");
		return;
	}

	if (update_ota_local(server, port) != 0)
		rt_printf("[RECOVERY] unable to start OTA\r\n");
}

static log_item_t recovery_commands[] = {
	{"ATRC", recovery_wifi},
	{"ATRO", recovery_update},
};

void recovery_at_init(void)
{
	log_service_add_table(recovery_commands,
			      sizeof(recovery_commands) / sizeof(recovery_commands[0]));
}

static int recovery_layout_is_valid(void)
{
	fw_img_export_info_type_t *info = get_fw_img_info_tbl();

	if (info == NULL) {
		rt_printf("[RECOVERY][FATAL] firmware table is unavailable\r\n");
		return 0;
	}

	rt_printf("[RECOVERY] loaded=%u fw1=0x%08lx fw2=0x%08lx\r\n",
		  (unsigned int)info->loaded_fw_idx,
		  (unsigned long)info->fw1_start_offset,
		  (unsigned long)info->fw2_start_offset);

	if ((info->loaded_fw_idx != 1U) ||
	    (info->fw1_start_offset != CARBOX_RECOVERY_FW_BASE) ||
	    (info->fw2_start_offset != CARBOX_MAIN_FW_BASE)) {
		rt_printf("[RECOVERY][FATAL] partition/runtime layout mismatch; OTA disabled\r\n");
		return 0;
	}

	return 1;
}

void main(void)
{
	console_init();
	rt_printf("\r\n[RECOVERY] immutable FW1 recovery image\r\n");

	LwIP_Init();
	if (wifi_on(RTW_MODE_STA) != RTW_SUCCESS) {
		rt_printf("[RECOVERY][FATAL] wifi_on failed\r\n");
	} else if (recovery_layout_is_valid()) {
		/* OTA writes are guarded and can modify only the fixed FW2 range. */
		rt_printf("[RECOVERY] commands: ATRC=SSID,PASSWORD; ATRO=IP,PORT\r\n");
	}

	vTaskStartScheduler();
	for (;;)
		;
}
