#include "uvc/inc/usbd_uvc_desc.h"
#include "isp_cmd.h"
#include "hal_api.h"
#include "hal_isp.h"
#include "isp_api.h"
#include "module_uvcd.h"
#include "ctype.h"
#include <rtsvideo.h>
#if !LIB_UVCD_DUAL
extern struct uvc_format *uvc_format_ptr;

static int g_bIsQualify = 0;
static int g_bIsGetDataFinished = 0;
static int g_bSpecialCMD = 0;
static int g_bIsCmdNULL = 0;
static int g_bIsSwitchFormatNV16 = 0; //0x803A
static u8 isp_send_cmd_static(struct isp_cmd_data *isp_data);

extern void set_isp_cmd(struct isp_cmd_data* a_ptISPCmd);

extern process_unit p_data;
extern process_unit x_data;
extern process_unit f_data;
extern isp_usbd_cmd_data i_data;

static int g_enable_pu_camera = 1;

#define CMD_DATA_MAX 64

//Tuning tool SUBCMD = bCMD
#define DBG_CMD                         0X00
#define DBG_XMEM_SUBCMD 		0X02
#define DBG_DEVICE_QUALIFY_SUBCMD	0X10
#define DBG_USB_CAMERA_ID               "AMEBAPRO"

#define UVCD_VC_ITT_CAMERA       0x01
#define UVCD_VC_PROCESS_UNIT     0x02
#define UVCD_VC_EXTENSION_UNIT   0x03

extern struct uvc_dev gUVC_DEVICE;

int SetEnablePU(int flag)
{
	if(flag==0)
		g_enable_pu_camera = 0;
	else if(flag==1)
		g_enable_pu_camera = 1;
	else
		return -1;
	
	return 0;
}

static u8 isp_send_cmd_static(struct isp_cmd_data *isp_data)
{
	int dMainCode = isp_data->cmdcode>>8;
	if(g_bIsCmdNULL)
		return 0;
	if(isp_data->cmdcode == 0xBF && isp_data->index == 0)
		return 0;
	if(isp_data->length>CMD_DATA_MAX)
		return 0;
	if(dMainCode<0 || dMainCode>0x20)
		return 0;
	printf("cmdcode:0x%X,index:0x%X,length:0x%X,param:0x%X,addr:0x%X\r\n", isp_data->cmdcode, isp_data->index, isp_data->length, isp_data->param, isp_data->addr);
	if((isp_data->cmdcode&0x80) == 0x00){
		printf("send data len = %d\r\n",isp_data->length);
		for(int i=0;i<isp_data->length;i++){
			printf("[%d]=0x%02X ",i,isp_data->buf[i]);
		}
		printf("\r\n");	
	}
	
	
	struct isp_cmd_data rcmd;
	memset(&rcmd,0,sizeof(struct isp_cmd_data));
	rcmd.buf = malloc(CMD_DATA_MAX);
	//------------------
	//printf("cmdcode\r\n");
	if(g_bIsSwitchFormatNV16 && (isp_data->cmdcode&0x80) == 0x00)
	{
		char bayer_type = (isp_data->buf[0]&0xF);
		printf("[DEBUG_BAYER_TYPE] %X  %X \r\n", isp_data->buf[0], bayer_type);
		if(bayer_type == 0x0A)
		{
			uvc_format_ptr->bayer_type = BAYER_TYPE_AFTER_DNO;
			uvc_format_ptr->format = FORMAT_TYPE_BAYER;
		}
		else if(bayer_type == 0x0B)
		{
			uvc_format_ptr->bayer_type = BAYER_TYPE_AFTER_LSC;
			uvc_format_ptr->format = FORMAT_TYPE_BAYER;
		}
		else if(bayer_type == 0x0C)
		{
			uvc_format_ptr->bayer_type = BAYER_TYPE_AFTER_BLC;
			uvc_format_ptr->format = FORMAT_TYPE_BAYER;
		}
		else if(bayer_type == 0x0D)
		{
			uvc_format_ptr->bayer_type = BAYER_TYPE_BEFORE_BLC;
			uvc_format_ptr->format = FORMAT_TYPE_BAYER;
		}
		else
			uvc_format_ptr->format = FORMAT_TYPE_YUY2;
		
		printf("uvc_format_ptr->bayer_type: %d \r\n", uvc_format_ptr->bayer_type);
		gUVC_DEVICE.change_parm_cb(uvc_format_ptr);
	}
	else
	isp_send_cmd(isp_data);
	//printf("cmdcode\r\n");
	
	if(isp_data->cmdcode&0x80){//For read data
//		printf("ATI1=0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X\r\n", (isp_data->cmdcode>>8), (isp_data->cmdcode%256), isp_data->index, isp_data->length, isp_data->param, isp_data->addr);
		printf("read data len = %d\r\n",isp_data->length);
		for(int i=0;i<isp_data->length;i++)
			printf("[%d]=0x%02X ",i,isp_data->buf[i]);
		printf("\r\n");
	}
	g_bIsSwitchFormatNV16 = 0;
	//------------------
	
	return 0;
}

#define ITT_CAMERA      0x01
#define PROCESS_UNIT    0X02
#define EXTENSION_UNIT  0X03
u8 acTemp[1024];

typedef struct roi_s{
	uint16_t x,y,w,h;
}roi_t;

__weak roi_t uvcd_roi_array[16] = { { 0, 0, 40, 20},
						{30, 40, 120, 20},
						{100, 200, 70, 30},
						{150, 20, 50, 20},
					  };
__weak int uvcd_roi_count = 4;


#include "hal_misc.h"
extern sys_cp_fw_info_t cp_fw_info;
int isp_doanload_ctrl = 0;
char *extension_download_buf = NULL;

void uvc_ext_download_reset()
{
	isp_doanload_ctrl = 0;
}

void uvc_ext_download_set_buf(void* download_buf)
{
	extension_download_buf = (char*)download_buf;
}

void uvc_events_process_control(struct uvc_dev *dev, struct usb_ctrlrequest *ctrl,
struct uvc_request_data *resp)
{
	//printf("control request (req %02x cs %02x)\n", req, cs);
	//(void)dev;
	//(void)resp;
	int i;
	struct isp_cmd_data cmd_data1;
	int sel = dev->control;
	//extension_download_buf = (char*)cp_fw_info.isp_fw.start_addr;
	resp->length = ctrl->wLength;
	dev->control = ctrl->wValue>>8;//stream 0:control 1:stream ,control for selector ex:brightness
	dev->command_interface = ctrl->wIndex &0xff;//0 for ocntrol 1 for streaming
	dev->command_entity = (ctrl->wIndex>>8)&0xff;//2 process unit 3 for extension unit
	//printf("[ProcessUnit][uvc_events_process_control] %d  %d  %d \r\n", dev->command_entity, dev->control, ctrl->bRequest);

	cmd_data1.index   = dev->control  ;
	cmd_data1.length  = resp->length ;
	cmd_data1.param   = 0;
	cmd_data1.addr    = 0;
	cmd_data1.buf     = resp->data;

	if(dev->command_entity == PROCESS_UNIT){
		if(sel==0x1 || sel==0x2 || sel==0x3 || (sel>=0x5 && sel<=0xc ))
		{
			cmd_data1.cmdcode = 0x0300+ctrl->bRequest;//0x0381;

			if(cmd_data1.cmdcode&0x80)
				if(g_enable_pu_camera)
					isp_send_cmd_static(&cmd_data1); //Results are in dev->isp_data.buf;
		}		
	}else if(dev->command_entity == ITT_CAMERA){
		if(sel==0x2 || sel==0x3 || sel==0x4 || sel==0xb || sel==0xd || sel==0xf)
		{
			cmd_data1.cmdcode = 0x0400+ctrl->bRequest;//0x0381;
			if(cmd_data1.cmdcode&0x80)
				if(g_enable_pu_camera)
					isp_send_cmd_static(&cmd_data1); //Results are in dev->isp_data.buf;
		}
	}else if(dev->command_entity == EXTENSION_UNIT){
		//}else if(ctrl->wIndex == EXTENSION_UNIT){
		//printf("[OSCAR_UVCD_GET]%X %d.\r\n", dev->control, ctrl->wLength);
		if(dev->control==VendorCommand || dev->control==VendorData)
		{
			switch (ctrl->bRequest) {
			case UVC_SET_CUR:
				break;
			case UVC_GET_CUR:
				{
					struct isp_cmd_data cmd_data;
					//memset(acTemp, 0, sizeof(acTemp));
					cmd_data.cmdcode = dev->isp_data.cmdcode;
					cmd_data.index   = dev->isp_data.index  ;
					cmd_data.length  = dev->isp_data.length ;
					cmd_data.param   = dev->isp_data.param  ;
					cmd_data.addr    = dev->isp_data.addr   ;
					cmd_data.buf     = acTemp;
					if( cmd_data.cmdcode == 0x4e4e){
						//NN command
						switch(cmd_data.index ){	
						case 0x10:// return number of ROI
							*(int*)resp->data = uvcd_roi_count;
							*(short*)((int)resp->data+4) = 640;
							*(short*)((int)resp->data+6) = 360;
							cmd_data.length = sizeof(int)+2*sizeof(short);
							break;
						case 0:
						case 1:
						case 2:
						case 3:	
						case 4:
						case 5:
						case 6:
						case 7:
						case 8:
						case 9:
						case 10:
						case 11:	
						case 12:
						case 13:
						case 14:
						case 15:
							// return index 0~15 ROI x,y,w,h
							memcpy(resp->data, &uvcd_roi_array[cmd_data.index&0xF], sizeof(roi_t)); 
							cmd_data.length = sizeof(roi_t);
							break;
						}


					}else{
						if(!g_bIsQualify)
						{
							if(g_bIsGetDataFinished)//---Once! Do the event before getting data!
							{
								if(g_bSpecialCMD)
								{
									if(cmd_data.index == 1)
									{
									}
									else if(cmd_data.index == 2)
									{
										cmd_data.length = cmd_data.param;
										printf("Special command API run (Download)\r\n");
										for(i=0; i<1024; i++)
											acTemp[i] = (1024-1-i)%256;
									}
								}
								else
									isp_send_cmd_static(&cmd_data); //Results are in dev->isp_data.buf;
								//memcpy(dev->isp_data.buf, acTemp, cmd_data.length);
								g_bIsGetDataFinished = 0;
							}
							memcpy(resp->data,cmd_data.buf+dev->isp_data.offset,ctrl->wLength);
						}
						else
						{
							memcpy(resp->data,DBG_USB_CAMERA_ID,ctrl->wLength);
						}


						//for(i=0;i<ctrl->wLength;i++)
						//	printf("buf[%d] = 0x%02x\r\n", i, resp->data[i]);
						dev->isp_data.offset+=ctrl->wLength;
						if(dev->isp_data.offset >= dev->isp_data.param)
						{
							g_bIsGetDataFinished = 1;
							//printf("Download Done! \r\n");
						}
					}
					break;
				}
			case UVC_GET_MIN:
				memcpy(resp->data,(unsigned char*)&x_data.min,ctrl->wLength);
				break;
			case UVC_GET_MAX:
				memcpy(resp->data,(unsigned char*)&x_data.max,ctrl->wLength);
				break;
			case UVC_GET_DEF:
				memcpy(resp->data,(unsigned char*)&x_data.def,ctrl->wLength);
				break;
			case UVC_GET_RES:
				memcpy(resp->data,(unsigned char*)&x_data.res,ctrl->wLength);
				break;
			case UVC_GET_LEN:
				memcpy(resp->data,(unsigned char*)&x_data.len,ctrl->wLength);
				break;
			case UVC_GET_INFO:
				memcpy(resp->data,(unsigned char*)&x_data.info,ctrl->wLength);
				break;
			}
		}
		else if(dev->control==UserDefineCommand || dev->control==UserDownloadData)
		{
			switch (ctrl->bRequest) {
			case UVC_SET_CUR:
				break;
			case UVC_GET_CUR:
				{
					if(extension_download_buf && dev->control==UserDownloadData)
					{
						memcpy(resp->data, extension_download_buf+isp_doanload_ctrl*ctrl->wLength, ctrl->wLength);
						//printf("Port: 0x%X.\r\n", dev->control);
						isp_doanload_ctrl++;
					}
					if(uvc_format_ptr->uvcd_ext_set_cb && dev->control==UserDefineCommand)
					{
						(*uvc_format_ptr->uvcd_ext_set_cb)(acTemp);
						//printf("Port: 0x%X(Set_CB) Data:%d  %d  %d.\r\n", dev->control, acTemp[0], acTemp[1], acTemp[2]);
						memcpy(resp->data, acTemp, 3);
					}
					break;
				}
			case UVC_GET_MIN:
				memcpy(resp->data,(unsigned char*)&f_data.min,ctrl->wLength);
				break;
			case UVC_GET_MAX:
				memcpy(resp->data,(unsigned char*)&f_data.max,ctrl->wLength);
				break;
			case UVC_GET_DEF:
				memcpy(resp->data,(unsigned char*)&f_data.def,ctrl->wLength);
				break;
			case UVC_GET_RES:
				memcpy(resp->data,(unsigned char*)&f_data.res,ctrl->wLength);
				break;
			case UVC_GET_LEN:
				memcpy(resp->data,(unsigned char*)&f_data.len,ctrl->wLength);
				break;
			case UVC_GET_INFO:
				memcpy(resp->data,(unsigned char*)&f_data.info,ctrl->wLength);
				break;
			}
		}
	}
}

void tuning_to_isp(isp_usbd_cmd_data *isp,UvcVendorCmd *tuning_data)
{
	isp->addr = tuning_data->address;
	//isp->cmdcode = (tuning_data->cmd<<8)|((tuning_data->subcmd&0x40)?0x81:0x01);
	if(tuning_data->subcmd&0x40){//SET
		isp->cmdcode = (tuning_data->cmd<<8)|(0X80)|(tuning_data->subcmd&0X3F);
	}else{//GET
		isp->cmdcode = (tuning_data->cmd<<8)|(tuning_data->subcmd&0X3F);
	}
	isp->param = 0;
	isp->index = 0;
	isp->length = tuning_data->length;
}

void uvc_control_process_data(struct uvc_dev *dev, struct uvc_request_data *data)
{
	struct isp_cmd_data cmd_data1;
	int sel = dev->control;
	g_bIsQualify = 0;
	g_bIsCmdNULL = 0;
	switch(dev->command_entity){
	case UVCD_VC_ITT_CAMERA:
		if(sel==0x2 || sel==0x3 || sel==0x4 || sel==0xb || sel==0xd || sel==0xf)
		{
			cmd_data1.cmdcode = 0x0401;
			cmd_data1.index   = dev->control  ;
			cmd_data1.length  = data->length ;
			cmd_data1.param   = 0;
			cmd_data1.addr    = 0;
			cmd_data1.buf     = data->data;
			if(g_enable_pu_camera)
				isp_send_cmd_static(&cmd_data1);
		}
		break;
	case UVCD_VC_PROCESS_UNIT:
		if(sel==0x1 || sel==0x2 || sel==0x3 || (sel>=0x5 && sel<=0xc))
		{
			cmd_data1.cmdcode = 0x0301;
			cmd_data1.index   = dev->control  ;
			cmd_data1.length  = data->length ;
			cmd_data1.param   = 0;
			cmd_data1.addr    = 0;
			cmd_data1.buf     = data->data;
			if(g_enable_pu_camera)
				isp_send_cmd_static(&cmd_data1);
		}
		break;
	case UVCD_VC_EXTENSION_UNIT:
		USBD_PRINTF("uvc_extension_unit,,len =%d cs = %d\r\n",data->length,dev->control);
		if(dev->control==UserDefineCommand)
		{
			//printf("Data:%d  %d  %d.\r\n", data->data[0], data->data[1], data->data[2]);
			//printf("Port: 0x%X(Get_CB).\r\n", dev->control);
			if(uvc_format_ptr->uvcd_ext_get_cb)
				(*uvc_format_ptr->uvcd_ext_get_cb)(data->data);
		}
		else if(dev->control==VendorCommand || dev->control==VendorData)
		{
			if(dev->control == VendorCommand){
				
				g_bIsGetDataFinished = 1;
				g_bSpecialCMD = 0;
				if(data->data[0]==0 && data->data[1]==0)
				{
					g_bIsCmdNULL = 1;
					printf("NULL Command\r\n");
				}
				else if(data->data[0]==0xFF && data->data[1]==0xFF)
				{
					g_bSpecialCMD = 1;
					memcpy(&dev->isp_data, data->data, 8);
					printf("[For Special Command] length:%d index:%d param:%d addr:%d\r\n", dev->isp_data.length, dev->isp_data.index, dev->isp_data.param, dev->isp_data.addr);
					
					dev->isp_data.cmdcode = 0xFF00;
					printf("[For Special Command]\r\n");
				}
				else if(data->data[0] == 0){//for TUNING TOOL 
					data->data[1] = data->data[1] -1;
					UvcVendorCmd *uvc_vendor = (UvcVendorCmd*)data->data;
					tuning_to_isp(&dev->isp_data,uvc_vendor);
					if(((dev->isp_data.cmdcode+1)&0X7F) == DBG_DEVICE_QUALIFY_SUBCMD)//Not for isp command
					{
						memcpy(dev->isp_data.buf,DBG_USB_CAMERA_ID,dev->isp_data.length);
						g_bIsQualify = 1;
					}
					printf("Tuning tool cmd\r\n");
				}else if(data->data[0]=='N'){
					// machine learning, human object detection
					memcpy(&dev->isp_data, data->data, 8);
					//printf("NN test cmd\r\n");
				}
				else{ //FOR COMMON ISP TOOL
					memcpy(&dev->isp_data, data->data, 8);
					dev->isp_data.cmdcode -= 0x0100;
					printf("Demo tool cmd\r\n");
				}
				if(dev->isp_data.addr == 0x803A)
				{
					g_bIsSwitchFormatNV16 = 1;
				}
				dev->isp_data.offset = 0;//To record the sequency data count
				
				if(dev->isp_data.length == 0)
				{
					isp_send_cmd_static((struct isp_cmd_data*)&dev->isp_data);
				}
				
			}else if(dev->control == VendorData){
				if((dev->isp_data.cmdcode&0x80) == 0x00)
				{//For set
					memcpy(dev->isp_data.buf+dev->isp_data.offset,data->data,data->length);
					dev->isp_data.offset+=data->length;
					
					//printf("[USBD_EVT_DBG] %d %d %d,  %c%c%c%c%c%c%c%c\r\n", dev->isp_data.offset, dev->isp_data.length, g_bSpecialCMD, data->data[0], data->data[1], data->data[2], data->data[3], data->data[4], data->data[5], data->data[6], data->data[7]);		  
					if(dev->isp_data.offset >= dev->isp_data.length && dev->isp_data.length>0)
					{
						if(g_bSpecialCMD)
						{
							char acKey1[8] = {'T','O','B','A','Y','E','R'};
							char acKey2[8] = {'T','O','I','S','P','I','M','G'};
							char acKey3[8] = {'L','D','C','O','N'};
							char acKey4[8] = {'L','D','C','O','F','F'};
							for(int i=0; i<8; i++)
								data->data[i] = toupper(data->data[i]);
							
							if(memcmp(acKey3, data->data, 5)==0)
							{
								uvc_format_ptr->ldc = 1;
								dev->change_parm_cb(uvc_format_ptr);
								printf("LDC-On\r\n");
							}
							else if(memcmp(acKey4, data->data, 6)==0)
							{
								uvc_format_ptr->ldc = 0;
								dev->change_parm_cb(uvc_format_ptr);
								printf("LDC-Off\r\n");
							}
							else if(uvc_format_ptr->format == FORMAT_TYPE_YUY2)
							{
								if(memcmp(acKey1, data->data, 7)==0 )
								{
									uvc_format_ptr->isp_format = ISP_FORMAT_BAYER_PATTERN;
									dev->change_parm_cb(uvc_format_ptr);
									printf("ToBayer\r\n");
								}
								else if(memcmp(acKey2, data->data, 8)==0)
								{
									uvc_format_ptr->isp_format = ISP_FORMAT_YUV422_SEMIPLANAR;
									dev->change_parm_cb(uvc_format_ptr);
									printf("ToISPIMG\r\n");
								}
								else
									printf("To-NONE\r\n");
							}
						}
						else
						{
							u8 acTemp[CMD_DATA_MAX];
							if(dev->isp_data.cmdcode==0x0807 && dev->isp_data.index==1 && dev->isp_data.length==8)
							{
								memcpy(acTemp, dev->isp_data.buf, dev->isp_data.length);
								rts_write_isp_osd_date(0, dev->isp_data.param, acTemp[0]*1000+acTemp[1]*100+acTemp[2]*10+acTemp[3], acTemp[4]*10+acTemp[5], acTemp[6]*10+acTemp[7]);
								printf("[OSD DATE] Current Date.\r\n");
							}
							else if(dev->isp_data.cmdcode==0x0808 && dev->isp_data.index==1 && dev->isp_data.length==8)
							{
								memcpy(acTemp, dev->isp_data.buf, dev->isp_data.length);
								rts_write_isp_osd_date(1, dev->isp_data.param, acTemp[0]*1000+acTemp[1]*100+acTemp[2]*10+acTemp[3], acTemp[4]*10+acTemp[5], acTemp[6]*10+acTemp[7]);
								printf("[OSD DATE] Next Date.\r\n");
							}
							else
							{
								struct isp_cmd_data cmd_data;
								cmd_data.cmdcode = dev->isp_data.cmdcode;
								cmd_data.index   = dev->isp_data.index  ;
								cmd_data.length  = dev->isp_data.length ;
								cmd_data.param   = dev->isp_data.param  ;
								cmd_data.addr    = dev->isp_data.addr   ;
								cmd_data.buf     = acTemp;
								
								memcpy(acTemp, dev->isp_data.buf, cmd_data.length);
								isp_send_cmd_static(&cmd_data); //Results are in dev->isp_data.buf;
							}
						}
					}
				}
			}else{
				USBD_PRINTF("Not supprot the command %d\r\n",dev->control);
			}
		}
		break;
	default:
		USBD_PRINTF("setting unknown control, length = %d\n", data->length);
		return;
	}
}
#endif