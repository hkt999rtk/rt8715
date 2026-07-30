#ifndef _MODULE_AUDIO_H
#define _MODULE_AUDIO_H

#include "mmf2_module.h"
#include "audio_api.h"

#define ENABLE_ASP 1

#define CONFIG_MMF_AUDIO_DEBUG 0
#define CONFIG_MMF_AUDIO_ATAF 1

#define CMD_AUDIO_SET_PARAMS     	MM_MODULE_CMD(0x00)  // set parameter
#define CMD_AUDIO_GET_PARAMS     	MM_MODULE_CMD(0x01)  // get parameter
#define CMD_AUDIO_SET_SAMPLERATE	MM_MODULE_CMD(0x02)
#define CMD_AUDIO_SET_WORDLENGTH	MM_MODULE_CMD(0x03)
#define CMD_AUDIO_SET_MICGAIN		MM_MODULE_CMD(0x04)

#define CMD_AUDIO_SET_ADC_GAIN          MM_MODULE_CMD(0x06)
#define CMD_AUDIO_SET_DAC_GAIN          MM_MODULE_CMD(0x07)
#define CMD_AUDIO_SET_RESET             MM_MODULE_CMD(0x08)

#define CMD_AUDIO_SET_AUDIO_STOP        MM_MODULE_CMD(0x0A)
#define CMD_AUDIO_SET_AUDIO_START       MM_MODULE_CMD(0x0B)
#define CMD_AUDIO_SET_TRX				MM_MODULE_CMD(0x0C)
#define CMD_AUDIO_SET_VOL				MM_MODULE_CMD(0x0D)

#define CMD_AUDIO_SET_NS_ENABLE			MM_MODULE_CMD(0x10)
#define CMD_AUDIO_SET_AEC_ENABLE		MM_MODULE_CMD(0x11)
#define CMD_AUDIO_SET_AGC_ENABLE		MM_MODULE_CMD(0x12)
#define CMD_AUDIO_SET_VAD_ENABLE		MM_MODULE_CMD(0x13)

#define CMD_AUDIO_RUN_NS				MM_MODULE_CMD(0x14)
#define CMD_AUDIO_RUN_AEC				MM_MODULE_CMD(0x15)
#define CMD_AUDIO_RUN_AGC				MM_MODULE_CMD(0x16)
#define CMD_AUDIO_RUN_VAD				MM_MODULE_CMD(0x17)

#define CMD_AUDIO_SET_AEC_LEVEL			MM_MODULE_CMD(0x18)

#define CMD_AUDIO_APPLY					MM_MODULE_CMD(0x20)  // for hardware module


typedef struct audio_param_s{
    audio_sr        sample_rate;	// ASR_8KHZ
	audio_wl        word_length;	// WL_16BIT
	audio_mic_gain  mic_gain;		// MIC_40DB
	
	int				channel;		// 1
	int				enable_aec;		// 0: off  1: on
	int				enable_ns;		// 0: off, 1: out 2: in 3: in/out
	int				enable_agc;		// 0: off, 1: output agc
	int				enable_vad;		// 0: off  1: input vad
	int				mix_mode;		// 0
    //...
}audio_params_t;

typedef struct audio_ctx_s{
	void*             parent;
	
	audio_t*          audio;
	
	audio_params_t    params;
	
	uint8_t			  inited_aec;
	uint8_t			  inited_ns;
	uint8_t			  inited_agc;
	uint8_t			  inited_vad;
	
	uint8_t			  run_aec;
	uint8_t			  run_ns;
	uint8_t			  run_agc;
	uint8_t			  run_vad;
	
	uint32_t          sample_rate;
	uint8_t           word_length; // Byte
	// for AEC
	TaskHandle_t      aec_rx_task;
	xSemaphoreHandle  aec_rx_done_sema;
}audio_ctx_t;


extern mm_module_t audio_module;
#endif
