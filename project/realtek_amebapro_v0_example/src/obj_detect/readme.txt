Example Description

	This patch describes how to implement human detection or face detetcion.


Usage for project\realtek_amebapro_v0_example:
	(a) Replace files:
			component\common\api\network\include\lwipopts.h
			component\common\api\at_cmd\atcmd_wifi.c
			project\realtek_amebapro_v0_example\src\main.c
			
	(b) Add files:
			project\realtek_amebapro_v0_example\src\obj_detect\IAR\libSkynetAPI_iot_1.4.9.0.a
			project\realtek_amebapro_v0_example\src\obj_detect\IAR\lib_obj_detect.a
			project\realtek_amebapro_v0_example\src\obj_detect\GCC\libSkynetAPI_iotD.a
			project\realtek_amebapro_v0_example\src\obj_detect\GCC\lib_obj_detect.a
			project\realtek_amebapro_v0_example\src\obj_detect\SKYNET_IOTAPI.h
			project\realtek_amebapro_v0_example\src\obj_detect\skynet_device.c
			project\realtek_amebapro_v0_example\src\obj_detect\skynet_wakeup.c
			project\realtek_amebapro_v0_example\src\obj_detect\module_skynet.c
			project\realtek_amebapro_v0_example\src\obj_detect\module_skynet.h
			project\realtek_amebapro_v0_example\src\obj_detect\module_obj_detect.c
			project\realtek_amebapro_v0_example\src\obj_detect\module_obj_detect.h
			project\realtek_amebapro_v0_example\src\obj_detect\object_detection.h
			project\realtek_amebapro_v0_example\src\obj_detect\object_detection_init.c
			project\realtek_amebapro_v0_example\src\obj_detect\object_detection_init.h


For IAR
	(1) Add module_skynet.h, module_skynet.c, skynet_device.c, skynet_wakeup.c, module_obj_detect.c, module_obj_detect.h,
	    object_detection_init.c, object_detection_init.h to "user" folder of application_is workspace in "Project_is".
	
	(2) Add "libSkynetAPI_iot_1.4.9.0.a" and "lib_obj_detect.a" to "lib" folder of application_is workspace in "Project_is".

For GCC
	(1) Add INCLUDES += -I../src/obj_detect

	(2) Add SRC_C += ../src/obj_detect/skynet_device.c
		SRC_C += ../src/obj_detect/skynet_wakeup.c
		SRC_C += ../src/obj_detect/module_skynet.c
		SRC_C += ../src/obj_detect/module_obj_detect.c
		SRC_C += ../src/obj_detect/object_detection_init.c

	(3) Add LIBFLAGS += ../src/obj_detect/GCC/libSkynetAPI_iotD.a
	    	LIBFLAGS += ../src/obj_detect/GCC/lib_obj_detect.a

Additional settings:
	
	(1) In object_detection_init.h, use the macro USE_SKYNET and USE_RTSP to select weither to stream with skynet or rtsp.

	(2) In module_obj_detect, command "CMD_OBJ_DETECT_HUMAN" is set to detect human. "CMD_OBJ_DETECT_FACE" is set to detect face.

	(3) With padding method, the last parameter of object_detection function in "module_obj_detect.c" should be set to 1.
	    object_detection(ctx->hold_image_address, ctx->params.width, ctx->params.height, 1, ctx->box.output_boxes, ctx->box.output_classes, ctx->box.output_scores, ctx->box.output_num_detections, 1);
	
	(4) Ch2 image size can be set by changing V2_WIDTH and V2_HEIGHT in "object_detection_init.h"
	    #define V2_WIDTH  224//320
	    #define V2_HEIGHT 224//180

To see the object detection result with skynet

	(1) Download app "Totokan" from app store.
	(2) Use atcmd enter ATUD=PIXMAX-XXXXXXXX-XXXXX,XXXXXX
	(3) Open "Totokan", and enter UID: PIXMAX-XXXXXXXX-XXXXX, password:12345678
