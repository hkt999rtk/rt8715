/******************************************************************************
 *
 * Copyright(c) 2007 - 2018 Realtek Corporation. All rights reserved.
 *
 ******************************************************************************/
#include "example_media_framework.h"

#define ENABLE_WRTC_VIDEO

#ifdef ENABLE_WRTC_PEER_MMFV2

void mmf2_example_webrtc_peer_init(void)
{
	
#ifdef ENABLE_WRTC_VIDEO
        
	// ------ Channel 1 video--------------
	isp_v1_ctx = mm_module_open(&isp_module);
	if(isp_v1_ctx){
		mm_module_ctrl(isp_v1_ctx, CMD_ISP_SET_PARAMS, (int)&isp_v1_params);
		mm_module_ctrl(isp_v1_ctx, MM_CMD_SET_QUEUE_LEN, V1_SW_SLOT);
		mm_module_ctrl(isp_v1_ctx, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_STATIC);
		mm_module_ctrl(isp_v1_ctx, CMD_ISP_APPLY, 0);	// start channel 0
	}else{
		rt_printf("ISP open fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}
        
        
	h264_v1_ctx = mm_module_open(&h264_module);
	if(h264_v1_ctx){
        h264_v1_params.profile = H264_BASE_PROFILE;
        h264_v1_params.level =H264ENCODER_LEVEL_3;
        h264_v1_params.idrHeader = 1;

		mm_module_ctrl(h264_v1_ctx, CMD_H264_SET_PARAMS, (int)&h264_v1_params);
		mm_module_ctrl(h264_v1_ctx, MM_CMD_SET_QUEUE_LEN, V1_H264_QUEUE_LEN);
		mm_module_ctrl(h264_v1_ctx, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_DYNAMIC);
		mm_module_ctrl(h264_v1_ctx, CMD_H264_INIT_MEM_POOL, 0);
		mm_module_ctrl(h264_v1_ctx, CMD_H264_APPLY, 0);
	}else{
		rt_printf("H264 open fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}
        
#endif

	audio_ctx = mm_module_open(&audio_module);
	if(audio_ctx){
		
		mm_module_ctrl(audio_ctx, CMD_AUDIO_SET_PARAMS, (int)&audio_params);
		mm_module_ctrl(audio_ctx, MM_CMD_SET_QUEUE_LEN, 6);
		mm_module_ctrl(audio_ctx, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_STATIC);
		mm_module_ctrl(audio_ctx, CMD_AUDIO_APPLY, 0);
	}else{
		rt_printf("audio open fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}
        
	g711e_ctx = mm_module_open(&g711_module);
	if(g711e_ctx){
		mm_module_ctrl(g711e_ctx, CMD_G711_SET_PARAMS, (int)&g711e_params);
		mm_module_ctrl(g711e_ctx, MM_CMD_SET_QUEUE_LEN, 6);
		mm_module_ctrl(g711e_ctx, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_STATIC);
		mm_module_ctrl(g711e_ctx, CMD_G711_APPLY, 0);
	
	}else{
		rt_printf("G711 open fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}
        

	//--------------webrtc peer---------------
       
	mmwrtc_peer_ctx = mm_module_open(&wrtc_peer_module);
	if(mmwrtc_peer_ctx){
      mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SET_SIG_HOST, (int)WRTC_PEER_SIG_SERVER_URL);
      mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SET_PORT, WRTC_PEER_SIG_SERVER_PORT);
#ifdef ENABLE_WRTC_VIDEO	

      mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SELECT_STREAM, 0);
      mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SET_PARAMS, (int)&wrtc_peer_v_params);
      mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SET_APPLY, 0);

      mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SELECT_STREAM, 1);
      mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SET_PARAMS, (int)&wrtc_peer_a_params);
      mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SET_APPLY, 0);

#else

      mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SELECT_STREAM, 0);
      mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SET_PARAMS, (int)&wrtc_peer_a_params);
      mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SET_APPLY, 0);
#endif		
                
		mm_module_ctrl(mmwrtc_peer_ctx, CMD_WRTC_PEER_SET_STREAMMING, ON);
		//mm_module_ctrl(rtsp2_ctx, MM_CMD_SET_QUEUE_LEN, 3);
		//mm_module_ctrl(rtsp2_ctx, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_STATIC);
	}else{
          printf("\n\r");
          printf("\n\r mmf2_exmaple_wrtc_peer wrtc_peer open fail");
          printf("\n\r");
          goto mmf2_exmaple_wrtc_peer_fail;
	}
	

#ifdef ENABLE_WRTC_VIDEO
	//--------------Link video h264 and audio ---------------------------
	siso_isp_h264_v1 = siso_create();
	if(siso_isp_h264_v1){
		siso_ctrl(siso_isp_h264_v1, MMIC_CMD_ADD_INPUT, (uint32_t)isp_v1_ctx, 0);
		siso_ctrl(siso_isp_h264_v1, MMIC_CMD_ADD_OUTPUT, (uint32_t)h264_v1_ctx, 0);
		siso_start(siso_isp_h264_v1);
	}else{
		rt_printf("siso_isp_h264_v1 open fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}
        
#endif

	siso_audio_g711 = siso_create();
	if(siso_audio_g711){
		siso_ctrl(siso_audio_g711, MMIC_CMD_ADD_INPUT, (uint32_t)audio_ctx, 0);
		siso_ctrl(siso_audio_g711, MMIC_CMD_ADD_OUTPUT, (uint32_t)g711e_ctx, 0);
		siso_start(siso_audio_g711);
	}else{
		rt_printf("siso1 open fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}


#ifdef ENABLE_WRTC_VIDEO	
#if 1   //Video + Audio
	miso_h264_g711_wrtc_peer = miso_create();
	if(miso_h264_g711_wrtc_peer){
		miso_ctrl(miso_h264_g711_wrtc_peer, MMIC_CMD_ADD_INPUT0, (uint32_t)h264_v1_ctx, 0);
		miso_ctrl(miso_h264_g711_wrtc_peer, MMIC_CMD_ADD_INPUT1, (uint32_t)g711e_ctx, 0);
		miso_ctrl(miso_h264_g711_wrtc_peer, MMIC_CMD_ADD_OUTPUT, (uint32_t)mmwrtc_peer_ctx, 0);
		miso_start(miso_h264_g711_wrtc_peer);
	}else{
		rt_printf("miso_h264_g711_wrtc fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}

#else
         //Video only
        
	siso_g711_wrtc_peer = siso_create();
	if(siso_g711_wrtc_peer){	
		siso_ctrl(siso_g711_wrtc_peer, MMIC_CMD_ADD_INPUT0, (uint32_t)h264_v1_ctx, 0);
		siso_ctrl(siso_g711_wrtc_peer, MMIC_CMD_ADD_OUTPUT, (uint32_t)mmwrtc_peer_ctx, 0);
		siso_start(siso_g711_wrtc_peer);
	}else{
		rt_printf("miso_h264_wrtc fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}
	
#endif
	
#else
        // Audio Only
        
	siso_g711_wrtc_peer = siso_create();
	if(siso_g711_wrtc_peer){
            siso_ctrl(siso_g711_wrtc_peer, MMIC_CMD_ADD_INPUT0, (uint32_t)g711e_ctx, 0);
            siso_ctrl(siso_g711_wrtc_peer, MMIC_CMD_ADD_OUTPUT, (uint32_t)mmwrtc_peer_ctx, 0);
            siso_start(siso_g711_wrtc_peer);
	}else{
            rt_printf("miso_g711_wrtc fail\n\r");
            goto mmf2_exmaple_wrtc_peer_fail;
	}
	
#endif

#if 1    
// RTP audio from webRTC peer
	mmwrtc_rtp_ctx = mm_module_open(&wrtc_rtp_module);
	if(mmwrtc_rtp_ctx){
		mm_module_ctrl(mmwrtc_rtp_ctx, CMD_WRTC_RTP_SET_PARAMS, (int)&rtp_g711d_params);
                mm_module_ctrl(mmwrtc_rtp_ctx, CMD_WRTC_RTP_FROM_WRTC_PEER, 1);	// streamming source
		mm_module_ctrl(mmwrtc_rtp_ctx, MM_CMD_SET_QUEUE_LEN, 12);
		mm_module_ctrl(mmwrtc_rtp_ctx, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_STATIC);
		mm_module_ctrl(mmwrtc_rtp_ctx, CMD_WRTC_RTP_APPLY, 0);
		mm_module_ctrl(mmwrtc_rtp_ctx, CMD_WRTC_RTP_STREAMING, 1);	// streamming on
	}else{
		rt_printf("wrtc_peer RTP open fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}
	
	// G711D
	g711d_ctx = mm_module_open(&g711_module);
	if(g711d_ctx){
		mm_module_ctrl(g711d_ctx, CMD_G711_SET_PARAMS, (int)&g711d_params);
		mm_module_ctrl(g711d_ctx, MM_CMD_SET_QUEUE_LEN, 8);
		mm_module_ctrl(g711d_ctx, MM_CMD_INIT_QUEUE_ITEMS, MMQI_FLAG_STATIC);
		mm_module_ctrl(g711d_ctx, CMD_G711_APPLY, 0);
	}else{
		rt_printf("wrtc_peer G711 open fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}
	
	siso_rtp_g711d = siso_create();
	if(siso_rtp_g711d){
		siso_ctrl(siso_rtp_g711d, MMIC_CMD_ADD_INPUT, (uint32_t)mmwrtc_rtp_ctx, 0);
		siso_ctrl(siso_rtp_g711d, MMIC_CMD_ADD_OUTPUT, (uint32_t)g711d_ctx, 0);
		siso_start(siso_rtp_g711d);
	}else{
		rt_printf("wrtc_peer siso_rtp_g711d open fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}
	
	
	
	siso_g711d_audio = siso_create();
	if(siso_g711d_audio){
		siso_ctrl(siso_g711d_audio, MMIC_CMD_ADD_INPUT, (uint32_t)g711d_ctx, 0);
		siso_ctrl(siso_g711d_audio, MMIC_CMD_ADD_OUTPUT, (uint32_t)audio_ctx, 0);
		siso_start(siso_g711d_audio);
	}else{
		rt_printf("wrtc_peer siso_g711d_audio open fail\n\r");
		goto mmf2_exmaple_wrtc_peer_fail;
	}
#endif
       
        
	
	return;
mmf2_exmaple_wrtc_peer_fail:
	
	return;
}

#endif

