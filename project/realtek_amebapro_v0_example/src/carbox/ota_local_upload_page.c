#include "ota_local_upload_page.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "diag.h"

/*
 * The OTA HTTP implementation is supplied by lib_usbsmart.a.  Its root page
 * is emitted as homepage_1 + formatted homepage_2 + homepage_3.  Replace the
 * final pointer with a persistent copy containing the local-upload script.
 */
extern char *homepage_3;

extern const uint8_t carbox_ota_upload_js_start[];
extern const uint8_t carbox_ota_upload_js_end[];

__asm__(
	".section .rodata.carbox_ota_upload_js,\"a\",%progbits\n"
	".balign 1\n"
	".global carbox_ota_upload_js_start\n"
	"carbox_ota_upload_js_start:\n"
	".incbin \"../src/carbox/web/update.js\"\n"
	".global carbox_ota_upload_js_end\n"
	"carbox_ota_upload_js_end:\n"
	".previous\n");

void carbox_ota_local_upload_page_install(void)
{
	static const char script_open[] = "<script>\n";
	static const char script_close[] = "\n</script>\n";
	const char *original = homepage_3;
	size_t original_len;
	size_t script_len;
	size_t total_len;
	char *page;
	char *dst;

	if (original == NULL) {
		rt_printf("[OTA_LOCAL] homepage unavailable\r\n");
		return;
	}

	original_len = strlen(original);
	script_len = (size_t)(carbox_ota_upload_js_end -
			      carbox_ota_upload_js_start);
	total_len = original_len + sizeof(script_open) - 1U + script_len +
		    sizeof(script_close);
	page = (char *)pvPortMalloc(total_len);
	if (page == NULL) {
		rt_printf("[OTA_LOCAL] allocation failed bytes=%lu\r\n",
			  (unsigned long)total_len);
		return;
	}

	dst = page;
	memcpy(dst, original, original_len);
	dst += original_len;
	memcpy(dst, script_open, sizeof(script_open) - 1U);
	dst += sizeof(script_open) - 1U;
	memcpy(dst, carbox_ota_upload_js_start, script_len);
	dst += script_len;
	memcpy(dst, script_close, sizeof(script_close));

	homepage_3 = page;
	rt_printf("[OTA_LOCAL] local .bin upload UI installed js=%luB\r\n",
		  (unsigned long)script_len);
}
