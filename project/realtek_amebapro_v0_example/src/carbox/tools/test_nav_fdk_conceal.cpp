/* Link against an out-of-tree HOST build of the firmware's exact FDK source.
 * Tests API sequence/output validity only, not target timing or audio quality.
 * g++ -I<FDK>/libAACenc/include -I<FDK>/libAACdec/include -I<FDK>/libSYS/include
 *     test_nav_fdk_conceal.cpp <build>/.libs/libfdk-aac.a -lm -o test
 */
#include "aacenc_lib.h"
#include "aacdecoder_lib.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
static void run(unsigned rate) {
    HANDLE_AACENCODER enc=NULL;assert(aacEncOpen(&enc,0,1)==AACENC_OK);
    assert(aacEncoder_SetParam(enc,AACENC_AOT,39)==AACENC_OK);
    assert(aacEncoder_SetParam(enc,AACENC_SAMPLERATE,rate)==AACENC_OK);
    assert(aacEncoder_SetParam(enc,AACENC_CHANNELMODE,MODE_1)==AACENC_OK);
    assert(aacEncoder_SetParam(enc,AACENC_BITRATE,64000)==AACENC_OK);
    assert(aacEncoder_SetParam(enc,AACENC_SBR_MODE,0)==AACENC_OK);
    assert(aacEncoder_SetParam(enc,AACENC_GRANULE_LENGTH,480)==AACENC_OK);
    assert(aacEncoder_SetParam(enc,AACENC_TRANSMUX,TT_MP4_RAW)==AACENC_OK);
    assert(aacEncEncode(enc,NULL,NULL,NULL,NULL)==AACENC_OK);
    AACENC_InfoStruct info={};assert(aacEncInfo(enc,&info)==AACENC_OK);
    assert(info.frameLength==480);
    HANDLE_AACDECODER dec=aacDecoder_Open(TT_MP4_RAW,1);assert(dec);
    UCHAR *asc=info.confBuf;UINT asclen=info.confSize;
    assert(aacDecoder_ConfigRaw(dec,&asc,&asclen)==AAC_DEC_OK);
    INT_PCM input[480],output[482];UCHAR packet[4096];unsigned pending=0,plc=0,valid=0;
    for(unsigned frame=0;frame<80;frame++){
        for(unsigned i=0;i<480;i++)input[i]=(INT_PCM)(12000*sin((frame*480+i)*2*3.141592653589793*440/rate));
        void *inptr=input,*outptr=packet;INT iid=IN_AUDIO_DATA,oid=OUT_BITSTREAM_DATA;
        INT isize=sizeof(input),osize=sizeof(packet),ielem=sizeof(INT_PCM),oelem=1;
        AACENC_BufDesc in={1,&inptr,&iid,&isize,&ielem},out={1,&outptr,&oid,&osize,&oelem};
        AACENC_InArgs ia={};AACENC_OutArgs oa={};ia.numInSamples=480;
        assert(aacEncEncode(enc,&in,&out,&ia,&oa)==AACENC_OK && oa.numOutBytes>0);
        if(frame==20 || (frame>=30 && frame<32) || (frame>=40 && frame<43)){pending++;continue;}
        for(unsigned n=0;n<pending;n++){
            output[0]=12345;output[481]=-12345;memset(output+1,0,480*sizeof(INT_PCM));
            AAC_DECODER_ERROR e=aacDecoder_DecodeFrame(dec,output+1,480,AACDEC_CONCEAL);
            assert(IS_OUTPUT_VALID(e));assert(output[0]==12345 && output[481]==-12345);
            unsigned nonzero=0;for(unsigned i=1;i<=480;i++)nonzero+=output[i]!=0;
            assert(nonzero>0);plc++;
        }
        pending=0;UINT size=oa.numOutBytes,left=size;UCHAR *p=packet;
        assert(aacDecoder_Fill(dec,&p,&size,&left)==AAC_DEC_OK && !left);
        output[0]=12345;output[481]=-12345;
        assert(aacDecoder_DecodeFrame(dec,output+1,480,0)==AAC_DEC_OK);
        assert(output[0]==12345 && output[481]==-12345);
        CStreamInfo *si=aacDecoder_GetStreamInfo(dec);assert(si->frameSize==480 && si->numChannels==1 && si->sampleRate==(int)rate);valid++;
    }
    assert(plc==6 && valid==74);aacDecoder_Close(dec);aacEncClose(&enc);
    printf("PASS: real FDK AAC-ELD %u mono 480; 1/2/3 lost frames, 6 valid nonzero substitutes, next decode recovers, output guards intact\n",rate);
}
int main(){run(44100);run(48000);}
