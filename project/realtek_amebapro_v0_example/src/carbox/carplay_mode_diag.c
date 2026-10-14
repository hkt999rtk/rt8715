#include <stdint.h>
#include <stdio.h>

/* AirPlayResponse.o owns these globals. modeState is a 32-byte array in
 * AirPlayClient.o; the first three words are used by the modes parser. */
extern int carplay_audio;
extern int carplay_video;
extern int modeState[8];

extern int __real_AirPlayResponse_GetInfoInfoResponse(const void *data,
						       uint32_t length);
extern void __real_AirPlayResponse_GetInfoChangeModes(const void *data,
						      uint32_t length);
extern int __real_AirPlayInfo_IsPaired(void);
extern void __real_acc_carplay_cb_audio_video_state(int audio, int video);
extern void __real_lib_carplay_show(void);
extern void __real_lib_carplay_stop(void);

static void mode_diag_state(const char *stage)
{
	printf("[MODE_DIAG] %s audio=%d video=%d state=%d,%d,%d\n",
	       stage, carplay_audio, carplay_video,
	       modeState[0], modeState[1], modeState[2]);
}

int __wrap_AirPlayInfo_IsPaired(void)
{
	int paired = __real_AirPlayInfo_IsPaired();
	printf("[MODE_DIAG] stored_pair_for_current_peer=%d\n", paired);
	return paired;
}

int __wrap_AirPlayResponse_GetInfoInfoResponse(const void *data,
						       uint32_t length)
{
	int result;
	printf("[MODE_DIAG] info enter data=%p length=%lu\n",
	       data, (unsigned long)length);
	mode_diag_state("info before");
	result = __real_AirPlayResponse_GetInfoInfoResponse(data, length);
	printf("[MODE_DIAG] info return result=%d\n", result);
	mode_diag_state("info after");
	return result;
}

void __wrap_AirPlayResponse_GetInfoChangeModes(const void *data,
						      uint32_t length)
{
	printf("[MODE_DIAG] change_modes enter data=%p length=%lu\n",
	       data, (unsigned long)length);
	mode_diag_state("change_modes before");
	__real_AirPlayResponse_GetInfoChangeModes(data, length);
	mode_diag_state("change_modes after");
}

void __wrap_acc_carplay_cb_audio_video_state(int audio, int video)
{
	printf("[MODE_DIAG] video_cb enter audio=%d video=%d\n",
	       audio, video);
	__real_acc_carplay_cb_audio_video_state(audio, video);
	puts("[MODE_DIAG] video_cb return");
}

void __wrap_lib_carplay_show(void)
{
	printf("[MODE_DIAG] show enter caller=%p\n", __builtin_return_address(0));
	mode_diag_state("show before");
	__real_lib_carplay_show();
	puts("[MODE_DIAG] show return");
}

void __wrap_lib_carplay_stop(void)
{
	printf("[MODE_DIAG] stop enter caller=%p\n", __builtin_return_address(0));
	mode_diag_state("stop before");
	__real_lib_carplay_stop();
	puts("[MODE_DIAG] stop return");
}
