#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int DiagSnPrintf(char *buf, size_t size, const char *fmt, ...)
{
	int ret;
	va_list ap;

	if (buf == NULL || size == 0U) {
		return 0;
	}

	va_start(ap, fmt);
	ret = vsnprintf(buf, size, fmt, ap);
	va_end(ap);

	return ret;
}


char *bt_get_local_name(void)
{
	return (char *)"bt_xxx";
}


#ifndef CARBOX_EXPERIMENTAL_SMART_A_LINK

char *acc_carplay_get_manufacturer(void)
{
	return (char *)"manufacturer_xxx";
}


char *acc_carplay_get_model(void)
{
	return (char *)"model_xxx";
}


int CSystemSetup_GetAccessoryType(void)
{
	return 5;
}

#endif
