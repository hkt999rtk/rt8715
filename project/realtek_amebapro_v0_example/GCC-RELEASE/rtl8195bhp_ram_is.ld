/* Linker script to configure memory regions. */

/* !! the include symbole may failed if the symbol file name is too long!! */
INCLUDE "../../../component/soc/realtek/8195b/misc/bsp/ROM/romsym_is.so"

/* Select External RAM type - 1: LPDDR SDRAM 2: PSRAM */
useERAM = 1;

INCLUDE "amebapro_config_is.ld"
INCLUDE "xip_fw.ld"

XIP_OFFSET = 0x40140 - 0x220;

XIP_ORIGIN = linkFW==1 ? (__ICFEDIT_region_XIP_FW1_FLASH_start__ + XIP_OFFSET) : 0;
XIP_ORIGIN = linkFW==2 ? (__ICFEDIT_region_XIP_FW2_FLASH_start__ + XIP_OFFSET) : XIP_ORIGIN;
XIP_ORIGIN = linkFW==3 ? (0x9BE00000 + 0x140) : XIP_ORIGIN;

XIP_END = linkFW==1 ? __ICFEDIT_region_XIP_FW1_FLASH_end__ : 0;
XIP_END = linkFW==2 ? __ICFEDIT_region_XIP_FW2_FLASH_end__ : XIP_END;
XIP_END = linkFW==3 ? 0x9C000000 : XIP_END;

XIP_LENGTH = XIP_END - XIP_ORIGIN;

  /* Uses part of RAM_SHARED as normal RAM.
   * Safe only when VOE/Video Encoder is disabled.
   */

RAM_ORIGIN = 0x20100A00;
RAM_END = 0x20200A00;
RAM_LENGTH = RAM_END - RAM_ORIGIN;

RAM_NC_ORIGIN = reserveVOE==1 ? 0x20124000 : 0x2013CC00;
RAM_NC_END = reserveVOE==1 ? 0x20128000 : 0x2013FC00;
RAM_NC_LENGTH = RAM_NC_END - RAM_NC_ORIGIN;

RAM_SHARED0_ORIGIN = reserveVOE==1 ? 0x20128000 : 0x2013FC00;
RAM_SHARED0_END =  0x2013FC00; 
RAM_SHARED0_LENGTH = RAM_SHARED0_END - RAM_SHARED0_ORIGIN;

MEMORY
{
  ITCM_RAM (rx) 		: ORIGIN = 0x00010000, LENGTH = 0x0002FFFF - 0x00010000
  
  /* The vector table, it must start with 256 bytes aligned address */
  VECTORS_RAM (rwx)     : ORIGIN = 0x20000000, LENGTH = 0x20000100 - 0x20000000
  /* 0x20000000 ~ 0x20000400 is reserved for C-Cut(and after) ROM code */
  DTCM_RAM (wrx) 		: ORIGIN = 0x20000400, LENGTH = 0x20010000 - 0x20000400
  
  /* 0x201000C0 ~ 0x201008FF is reserved for ROM code */

  /* RAM functions entry table */
  RAM_FUN_TABLE (rwx)   : ORIGIN = 0x20100900, LENGTH = 0x201009F0 - 0x20100900

  /* RAM image Signature */
  RAM_IMG_SIGN (rwx)    : ORIGIN = 0x201009F0, LENGTH = 0x20100A00 - 0x201009F0
    
  /* Internal SRAM for text, rodata & data */
  RAM (rwx)             : ORIGIN = RAM_ORIGIN, LENGTH = RAM_LENGTH

  /* Non-Cacheable memory, defined by MPU configuration */
  RAM_NC (rwx)          : ORIGIN = RAM_NC_ORIGIN, LENGTH = RAM_NC_LENGTH

  /* Following RAM(96K) is shared by CPU and Video Encoder, CPU can use it only when Video Encoder is off */
  RAM_SHARED0 (rwx)      : ORIGIN = 0x20128000, LENGTH = RAM_SHARED0_LENGTH

  /* 0x2013FC00 ~ 0x2013FFFF is reserved for B-Cut(and before) ROM code */

  /* Following RAM(96K + 136K) is shared by CPU and Video Encoder, CPU can use it only when Video Encoder is off */
  RAM_SHARED (rwx)      : ORIGIN = RAM_SHARED0_ORIGIN, LENGTH = 0x2017A000 - 0x20140000

  /* PSRAM memory */
  PSRAM (rwx) 		    : ORIGIN = 0x60000000, LENGTH = 0x60800000 - 0x60000000

  /* LPDDR memory */
  RAM_LPDDR (rwx) 		: ORIGIN = 0x70000000, LENGTH = 0x72000000 - 0x70000000

  /* ERAM memroy */
  EXTERNAL_RAM (rwx)    : ORIGIN = useERAM==2 ? ORIGIN(PSRAM) : ORIGIN(RAM_LPDDR), LENGTH = useERAM==2 ? LENGTH(PSRAM) : LENGTH(RAM_LPDDR)

  /* Flash memory for XIP */
  /* XIP image must start with 256K(0x40000) aligned address */
   
  XIP_FLASH (rx) 		: ORIGIN = XIP_ORIGIN, LENGTH = XIP_LENGTH
 
  
}

/* Library configurations */
GROUP(libgcc.a libc.a libm.a libnosys.a)

/* Linker script to place sections and symbol values. Should be used together
 * with other linker script that defines memory regions FLASH and RAM.
 * It references following symbols, which must be defined in code:
 *   Reset_Handler : Entry of reset handler
 *
 * It defines following symbols, which code can use without definition:
 *   __exidx_start
 *   __exidx_end
 *   __copy_table_start__
 *   __copy_table_end__
 *   __zero_table_start__
 *   __zero_table_end__
 *   __etext
 *   __data_start__
 *   __preinit_array_start
 *   __preinit_array_end
 *   __init_array_start
 *   __init_array_end
 *   __fini_array_start
 *   __fini_array_end
 *   __data_end__
 *   __bss_start__
 *   __bss_end__
 *   __end__
 *   end
 *   __HeapLimit
 *   __StackLimit
 *   __StackTop
 *   __stack
 *   __Vectors_End
 *   __Vectors_Size
 */
_start_addr = 0x000000c0;
ENTRY(_start_addr)

__sram_end__ = ORIGIN(RAM) + LENGTH(RAM);
__eram_end__ = ORIGIN(EXTERNAL_RAM) + LENGTH(EXTERNAL_RAM);

SECTIONS
{
	.ram.vector :
	{
		. = ALIGN(256);
		__ram_vector_start__ = .;
		KEEP(*(.ram_vector_table))
		__ram_vector_end__ = .;
		. = ALIGN(4);
	} > VECTORS_RAM

	.ram.func.table :
	{
		. = ALIGN(32);
		__ram_start_table_start__ = .;
		KEEP(*(SORT(.start.ram.data*)))
		__ram_start_table_end__ = .;

	} > RAM_FUN_TABLE

	.ram.img.signature :
	{
		__ram_img_signature__ = .;
		KEEP(*(.start.ram.sign*))		
	} > RAM_IMG_SIGN

	/*******************************************************************************************/
	/*   ITCM/DTCM placement                                                                   */
	/*******************************************************************************************/

	.itcm_ram.code_rodata :
	{
		__itcm_ram_code_rodata_start__ = .;

		/*
		 * WLAN receive processing is a sustained hot path.  The prebuilt WLAN
		 * library marks it as .wlan.text.*, which would normally execute from
		 * external LPDDR.  Keep only the packet receive/validate/reorder/
		 * indicate functions in ITCM; the selected set is about 4.5 KiB and
		 * leaves ample ITCM headroom for the existing codec/network code.
		 */
		*(.wlan.text.rtl8195b_recv_tasklet)
		*(.wlan.text.rtw_recv_tasklet)
		*(.wlan.text.rtw_recv_entry)
		*(.wlan.text.recv_func)
		*(.wlan.text.recv_func_prehandle)
		*(.wlan.text.recv_func_posthandle)
		*(.wlan.text.validate_recv_frame)
		*(.wlan.text.check_indicate_seq)
		*(.wlan.text.enqueue_reorder_recvframe)
		*(.wlan.text.recv_indicatepkts_in_order)
		*(.wlan.text.recv_indicatepkt_reorder)
		*(.wlan.text.process_recv_indicatepkts)
		*(.wlan.text.amsdu_to_msdu)
		*(.wlan.text.rtw_recv_indicatepkt)
		*(.wlan.text.rltk_netif_rx)

		/* Source-owned GDMA RX rotation is called directly by the receive
		 * tasklet.  Its complete executable/constant footprint is about 1 KiB;
		 * keep it with the surrounding WLAN receive path in ITCM. */
		*lib_wlan_zero_copy.a:wlan_rx_dma_mgr.o(.text* .rodata*)

		/* Called at every context switch; keep the DWT read beside the
		 * FreeRTOS scheduler in ITCM and avoid an XIP/DRAM veneer. */
		*(.cinit.itcm.text.atss_runtime_counter_get)
		/* CINIT compilation prefixes explicit sections with `.cinit`. */
		*(.cinit.itcm.text.libc_memcpy_m33)

		/* Small source-owned crypto alias wrappers are on every protected
		 * audio packet.  Keep lookup/copy helpers beside the existing hot
		 * crypto wrappers rather than fetching them from NOR. */
		*chacha_key_alias_fix.o(.text* .rodata*)

		/* Repeated PC samples from the audio and WLAN tasks land in these
		 * compact time, allocation-accounting and arithmetic helpers. */
		*FreeRTOS_POSIX_clock.o(.text* .rodata*)
		*FreeRTOS_POSIX_utils.o(.text* .rodata*)
		*osdep_service.o(.text* .rodata*)
		*libgcc.a:_aeabi_ldivmod.o(.text*)
		*libgcc.a:_aeabi_uldivmod.o(.text*)
		*lib_SystemLib.a:Window.o(.text.GetTickCount)

		/*
		 * The public mem* wrappers deliberately avoid the slower Realtek
		 * mask-ROM utility stubs.  memcpy now has a measured M33 implementation
		 * in .itcm.text; retain newlib's other three primitives in ITCM.
		 */
		*libc.a:lib_a-memcmp.o(.text*)
		*libc.a:lib_a-memmove.o(.text*)
		*libc.a:lib_a-memset.o(.text*)

		*(.itcm.text*)

		/*
		 * The production AAC-LC route uses Helix and retains FDK only as a
		 * compatibility fallback/encoder.  Do not spend ITCM on the 50 KiB
		 * selected FDK subset; the complete FDK archive is placed in LPDDR
		 * below.  Helix executable sections remain in ITCM, while its lookup
		 * tables are separated into internal SRAM data sections later.
		 */

		/*
		 * Opus: CELT/SILK decode plus the two dominant top-level encoder
		 * loops.  The archive currently contributes code only when an Opus
		 * API is referenced; these rules do not force unused Opus into the
		 * production image.
		 */
		*lib_opus.a:*(
			.text.opus_decode_frame
			.text.celt_decode_with_ec
			.text.celt_synthesis
			.text.deemphasis*
			.text.silk_Decode
			.text.silk_decode_frame
			.text.silk_decode_core
			.text.silk_decode_parameters
			.text.silk_decode_indices
			.text.silk_decode_pulses
			.text.decode_pulses
			.text.quant_all_bands
			.text.unquant_coarse_energy
			.text.opus_fft_impl
			.text.opus_ifft_c
			.text.clt_mdct_backward_c
			.text.opus_encode_native
			.text.celt_encode_with_ec
		)

		/*
		 * CDC-NCM TX runs synchronously in the TCP_IP output path.  The
		 * customer archive is transformed into lib_usbsmart_link.a before it
		 * is linked, so match both that archive and an untransformed customer
		 * archive.  Keep the small per-packet path in ITCM; the rest of the USB
		 * stack is placed in LPDDR below.
		 */
		*lib_usbsmart*.a:usbh_cdc_ncm_hal.o(.text.usbh_cdc_ncm_send_data)
		*lib_usbsmart*.a:ncm.o(.text.ncm_wrap_ntb)
		*lib_usbsmart*.a:usbh_cdc_ncm.o(.text.usbh_cdc_ncm_bulk_send)
		*(.text.__wrap_ncm_wrap_ntb)
		*ncm_wrap_profiler.o(.text.ncm_wrap_copy_elide*)
		*(.eram.text.ncm_send_pbuf_sync)
		*(.eram.text.ncm_tx_async_worker)
		*lib_usbsmart*.a:usbh_cdc_ncm.o(
			.text.usbh_cdc_ncm_bulk_tx
			.text.usbh_cdc_ncm_process_bulk_out
		)

		/* The USB host ISR and its small direct completion helpers execute for
		 * every bulk transfer.  Keep this bounded set in ITCM; enumeration,
		 * setup and class-control paths remain in LPDDR. */
		*lib_usbsmart*.a:usbh_hcd.o(
			.text.usbh_hcd_irq_handler
			.text.usbh_hcd_isr_task
			.text.usbh_hcd_hc_get_urb_state
			.text.usbh_hcd_handle_*
			.text.usbh_hcd_release_channel*
			.text.usbh_hcd_save_data_toggle*
			.text.usbh_hcd_hc_halt
			.text.usbh_hcd_hc_submit_request
		)
		*lib_usbsmart*.a:usbh_hal.o(
			.text.usbh_hal_hc_halt
			.text.usbh_hal_hc_read_interrupt
			.text.usbh_hal_init_hc_int_mask
			.text.usbh_hal_hc_start_transfer
			.text.usbh_hal_init_clock
		)
		*lib_usbsmart*.a:usb_hal.o(
			.text.usb_hal_disable_interrupt
			.text.usb_hal_read_interrupts
			.text.usb_hal_read_packet
			.text.usb_hal_write_packet
			.text.usb_hal_get_otg_mode
			.text.usb_hal_enable_interrupt
		)
		*lib_usbsmart*.a:usb_os.o(
			.text.usb_os_lock
			.text.usb_os_unlock
			.text.usb_os_sema_take
		)
		*lib_usbsmart*.a:usbh_core.o(
			.text.usbh_core_enable_port
			.text.usbh_core_disable_port
			.text.usbh_core_notify_urb_state_change
			.text.usbh_core_get_urb_state
			.text.usbh_core_bulk_send_data
			.text.usbh_core_get_time_tick_stamp
		)
		*lib_usbsmart*.a:usbh.o(
			.text.usbh_get_urb_state
			.text.usbh_get_elapsed_ticks
			.text.usbh_get_tick
			.text.usbh_bulk_send_data
		)
		*(.text.DCache_Clean)
		*(.text.DCache_Invalidate)
		/* The hardware IRQ entry wakes the HCD task through this compatibility
		 * wrapper.  Keeping both tiny functions here avoids an LPDDR/NOR fetch
		 * on every USB interrupt. */
		*(.text.__wrap_usb_os_sema_give)
		*(.text.carbox_libusb_ref_yield_from_isr)

		/* lwIP's owned write and output functions are the synchronous bridge
		 * from application-owned payloads to the NCM packet path. */
		*(.eram.text.tcp_write_owned)
		*(.eram.text.tcp_output)
		*(.eram.text.tcp_output_control_segment)
		*(.eram.text.tcp_owned_pbuf_free)

		*(.haac.text.text*)
		*(.faac.text*)
		*(.hmp3.text*)

		__itcm_ram_code_rodata_end__ = .;
	} > ITCM_RAM

	.dtcm_ram.bss :
	{
		/* Note: Boot loader data buffer & stack is allocated in DTCM RAM !! */
		/*       so do not allocate data/rodata/text in this section, it may cause boot loader failed */
		__dtcm_ram_bss_start__ = .;

		*(.dtcm_ram.bss*)
		/* *(.itcm.bss.mem*) */
		
		
		__dtcm_ram_bss_end__ = .;
	} > DTCM_RAM


	/*******************************************************************************************/
	/*   External RAM placement                                                                */
	/*******************************************************************************************/	
	.eram.code_text :
	{
		. = ALIGN(16);
		__eram_code_text_start__ = .;
		__psram_code_text_start__ = .;
		__lpddr_code_text_start__ = .;

		*(.psram.text*)
		*(.lpddr.text*)
		*(.sdram.text*)
		*(.usbd.text*)
		*(.wlan.text*)
		*(.eram.text*)
		*(.speex.text*)

		/* Compatibility code participates in the USB runtime path.  Hot leaf
		 * functions selected above live in ITCM; keep every remaining wrapper
		 * in LPDDR so USB execution never falls back to NOR XIP. */
		*libusb_ref_compat_hal.o(.text*)
		*libusb_ref_compat_os.o(.text*)
		*usb_ref_smart_compat.o(.text*)
		*usbsmart_otp_compat.o(.text*)
		*carplay_smart_api_stubs.o(.text*)

		/* Keep the sustained USB host controller path out of NOR XIP. */
		*lib_usbsmart*.a:usbh_hcd.o(.text*)
		*lib_usbsmart*.a:usbh_hal.o(.text*)
		*lib_usbsmart*.a:usb_hal.o(.text*)
		*lib_usbsmart*.a:usb_os.o(.text*)
		/*
		 * Keep the complete USB stack out of NOR XIP.  Sections selected for
		 * ITCM above have already been consumed; every remaining USB function
		 * executes from LPDDR.
		 */
		*lib_usbsmart*.a:*(.text*)

		/*
		 * Sustained CarPlay screen/audio processing must not compete with
		 * NOR XIP cache misses.  ITCM input sections were consumed above and
		 * retain priority; place only the remaining ordinary text in LPDDR.
		 * The CarPlay archive also contains the ChaCha/AES routing and software
		 * fallback paths used by AirPlayScreenReceiver.
		 */
		*lib_CarPlay.a:*(.text*)
		*lib_CarPlay_chacha_m33.a:*(.text*)
		*lib_fdkaac*.a:*(.text*)
		*lib_Accessory2.a:AirPlayScreen.o(.text*)

		/* The active build links handover-suffixed CarPlay archives.  Keep the
		 * sustained screen receive/dispatch path out of NOR without pulling the
		 * complete (roughly 1 MiB) handover archives into LPDDR. */
		*lib_Accessory2_handover.a:AirPlayScreen.o(.text*)
		*lib_CarPlay_chacha_m33_handover.a:AirPlayReceiverSessionScreen.o(.text*)
		*lib_CarPlay_chacha_m33_handover.a:ScreenUtilsStub.o(.text*)
		*lib_CarPlay_chacha_m33_handover.a:AirPlayReceiverSession.o(.text._ScreenThread)

		/* Sustained audio decode/output path from the same handover archives.
		 * AirPlayReceiverSession.o is large, so select only its audio receive
		 * function instead of relocating the complete object. */
		*lib_Accessory2_handover.a:AirPlayAudio.o(.text*)
		*lib_CarPlay_chacha_m33_handover.a:AudioConverter_FDKAAC_OPUS.o(.text*)
		*lib_CarPlay_chacha_m33_handover.a:AudioUtilsALSA.o(.text*)
		*lib_CarPlay_chacha_m33_handover.a:AirPlayReceiverSession.o(.text.AirPlayReceiverSessionReadAudio)

		/* PC sampling shows the active audio output path repeatedly executing
		 * these software ChaCha/Poly1305 sections from NOR.  Relocate only the
		 * observed functions; the much larger fallback object stays in XIP. */
		*lib_CarPlay_chacha_m33_handover.a:ChaCha20Poly1305.o(
			.text.chacha20_poly1305_xor
			.text.chacha20_poly1305_finish_tag
			.text.chacha20_poly1305_final
			.text.poly1305_blocks_c
			.text.chacha20_poly1305_init_64x64_software
			.text.chacha_select_hardware_backend
			.text.chacha_deferred_copy
			.text.chacha_hardware_xor_chunks
			.text.chacha_hardware_auth_standalone
			.text.poly1305_update_state
			.text.chacha20_poly1305_init_64x64
			.text.chacha20_poly1305_add_aad
			.text.chacha20_poly1305_encrypt
			.text.chacha20_poly1305_decrypt
			.text.chacha20_poly1305_verify
			.text.chacha_mode2_finish_encrypt_software
		)
		*lib_CarPlay_chacha_m33_handover.a:MiscUtils.o(.text.Swap16Mem)

		/* Default executable placement: after the explicit ITCM and LPDDR
		 * selectors above, put every remaining ordinary text section in
		 * LPDDR.  The excluded objects are consumed later by the internal-RAM
		 * and C-init output sections; they must remain executable while flash
		 * or LPDDR clocks/controllers are being initialized or reconfigured. */
		*(EXCLUDE_FILE (
			*hal_spic.o
			*hal_lpddr.o
			*hal_misc.o
			*hal_sys_ctrl.o
			*ram_start.o
			*_arm_addsubdf3.o
			*_arm_muldivdf3.o
			*_arm_fixunsdfsi.o
		) .text*)

		__psram_code_text_end__ = .;
		__lpddr_code_text_end__ = .;
	} > EXTERNAL_RAM

	.eram.code_rodata :
	{
		. = ALIGN(16);
		__eram_code_rodata_start__ = .;
		__psram_code_rodata_start__ = .;
		__lpddr_code_rodata_start__ = .;

		*(.psram.rodata*)
		*(.lpddr.rodata*)
		*(.sdram.rodata*)
		*(.usbd.rodata*)
		*(.wlan.rodata*)
		*(.eram.rodata*)

		*(.psram.rodata.str1*)
		*(.lpddr.rodata.str1*)
		*(.sdram.rodata.str1*)
		*(.usbd.rodata.str1*)
		*(.wlan.rodata.str1*)
		*(.eram.rodata.str1*)
		*libusb_ref_compat_hal.o(.rodata*)
		*libusb_ref_compat_os.o(.rodata*)
		*usb_ref_smart_compat.o(.rodata*)
		*usbsmart_otp_compat.o(.rodata*)
		*carplay_smart_api_stubs.o(.rodata*)

		*lib_usbsmart*.a:usbh_hcd.o(.rodata*)
		*lib_usbsmart*.a:usbh_hal.o(.rodata*)
		*lib_usbsmart*.a:usb_hal.o(.rodata*)
		*lib_usbsmart*.a:usb_os.o(.rodata*)
		*lib_usbsmart*.a:*(.rodata*)

		/* Keep hot codec/crypto lookup tables with their LPDDR text. */
		*lib_CarPlay.a:*(.rodata*)
		*lib_CarPlay_chacha_m33.a:*(.rodata*)
		*lib_fdkaac*.a:*(.rodata* .haac.rodata* .faac.rodata*)
		*lib_Accessory2.a:AirPlayScreen.o(.rodata*)

		*lib_Accessory2_handover.a:AirPlayScreen.o(.rodata*)
		*lib_CarPlay_chacha_m33_handover.a:AirPlayReceiverSessionScreen.o(.rodata*)
		*lib_CarPlay_chacha_m33_handover.a:ScreenUtilsStub.o(.rodata*)
		*lib_CarPlay_chacha_m33_handover.a:AirPlayReceiverSession.o(.rodata._ScreenThread*)

		*lib_Accessory2_handover.a:AirPlayAudio.o(.rodata*)
		*lib_CarPlay_chacha_m33_handover.a:AudioConverter_FDKAAC_OPUS.o(.rodata*)
		*lib_CarPlay_chacha_m33_handover.a:AudioUtilsALSA.o(.rodata*)
		*lib_CarPlay_chacha_m33_handover.a:AirPlayReceiverSession.o(.rodata.AirPlayReceiverSessionReadAudio*)
		*lib_CarPlay_chacha_m33_handover.a:ChaCha20Poly1305.o(
			.rodata.chacha20_poly1305_final*
			.rodata.chacha20_poly1305_init_64x64*
			.rodata.chacha20_poly1305_verify*
			.rodata.zeros.*
		)

		/* Default constants follow their code into LPDDR.  Preserve the
		 * controller/startup objects selected by .ram.code_rodata below. */
		*(EXCLUDE_FILE (
			*hal_spic.o
			*hal_lpddr.o
			*hal_misc.o
			*hal_sys_ctrl.o
			*ram_start.o
		) .rodata*)
		*(.itcm.rodata*)
		*(.haac.rodata*)
		*(.faac.rodata*)
		*(.speex.rodata*)

		. = ALIGN(4);
		__psram_code_rodata_end__ = .;
		__lpddr_code_rodata_end__ = .;
	} > EXTERNAL_RAM

	.eram.data : AT (__eram_etext)
	{
		. = ALIGN(16);
	    __eram_etext = .; 
		__psram_data_start__ = .;
		__lpddr_data_start__ = .;

		*(.psram.data*)
		*(.lpddr.data*)
		/* screen_tx_direct_crypto.o is SRAM-prefixed as a whole; only its
		 * cold 10-second SCREENBLOCK accumulator belongs in LPDDR. */
		*(.sram.lpddr.data.screen_block_stats)
		*(.sdram.data*)
		*(.usbd.data*)
		*(.wlan.data*)
		/* tcpip.o owns the per-packet RX stage-1 pool and its small free-list
		 * state.  Leave those sections for the internal-SRAM data output below;
		 * all other ERAM data remains in LPDDR. */
		*(EXCLUDE_FILE (*tcpip.o) .eram.data*)
		*/mem.o (.itcm.bss*)
		*/memp.o (.itcm.bss*)

		__psram_data_end__ = .;
		__lpddr_data_end__ = .;
	} > EXTERNAL_RAM

	.eram.bss (NOLOAD) :
	{
		. = ALIGN(16);
		*(.lpddr.bss.noinit*)

		__eram_bss_start__ = .;
		__psram_bss_start__ = .;
		__lpddr_bss_start__ = .;

		*(.psram.bss*)
		*(.lpddr.bss*)
		/* SRAM_C adds a .sram prefix to input sections.  Allow explicitly
		 * tagged, non-critical statistics to remain in LPDDR while the IRQ
		 * controller's live semaphore/state stays in internal SRAM. */
		*(.sram.lpddr.bss*)
		*(.sdram.bss*)
		*(.usbd.bss*)
		*(.wlan.bss*)
		*(EXCLUDE_FILE (*tcpip.o) .eram.bss*)
		*(.bss.eram*)

		__eram_bss_end__ = .;
		__psram_bss_end__ = .;
		__lpddr_bss_end__ = .;
	} > EXTERNAL_RAM
	

	/*******************************************************************************************/
	/*   Internal RAM placement                                                                */
	/*******************************************************************************************/	
	.ram.code_text :
	{
		. = ALIGN(32);
		__ram_entry_text_start__ = .;
		*(.ram_entry_func.text*)
		__ram_entry_text_end__ = .;
		
		. = ALIGN(32);
		__ram_code_text_start__ = .;

		/* SPI flash controller related code should be located in SRAM, never locates them in XIP */
		*hal_spic.o(.text*)

		/* LPDDR controller related code should be located in SRAM, never locates them in LPDDR */
		*hal_lpddr.o(.text*)

		*hal_misc.o(.text*)
		*hal_sys_ctrl.o(.text*)
		*ram_start.o(.text*)
		/* *libgcc.a:*.o(.text*) */

		*(.sram.text*)
		
		. = ALIGN(4);
		__ram_code_text_end__ = .;
	} > RAM

	.ram.code_rodata :
	{
		. = ALIGN(16);
		__ram_code_rodata_start__ = .;

		/* SPI flash controller related code should be located in SRAM, never locates them in XIP */		
		*hal_spic.o(.rodata*)

		/* LPDDR controller related code should be located in SRAM, never locates them in LPDDR */
		*hal_lpddr.o(.rodata*)

		*hal_misc.o(.rodata*)
		*hal_sys_ctrl.o(.rodata*)
		*ram_start.o(.rodata*)

		/* Helix AAC constant lookup tables are read for every decoded frame.
		 * Keep them in internal SRAM without consuming instruction TCM. */
		*(.haac.text.rodata*)

		*(.sram.rodata*)
		*(.sram.rodata.str1*)
		. = ALIGN(4);
		__ram_code_rodata_end__ = .;
	} > RAM

	.ram.data :
	{
		. = ALIGN(16);
		__fw_img_start__ = .;
		__data_start__ = .;
		/* The vendor Helix archive labels large twiddle/window/Huffman tables
		 * as sdram.data below a .haac.text prefix.  They are hot read-mostly
		 * data, so place them in internal SRAM rather than ITCM or LPDDR. */
		*(.haac.text.sdram.data*)
		/* RX stage-1 free-list indices are touched with the pool for every
		 * packet.  Keep them in the same internal-SRAM bank. */
		*tcpip.o(.eram.data*)
		*(vtable)
		*(.data*)
		*(.sram.data*)
		*(.itcm.data*)
		*(.haac.data*)
		*(.faac.data*)
		*(.speex.data*)

		. = ALIGN(4);
		/* preinit data */
		PROVIDE_HIDDEN (__preinit_array_start = .);
		KEEP(*(.preinit_array))
		PROVIDE_HIDDEN (__preinit_array_end = .);

		. = ALIGN (4);
		__initcall_start = .;
		KEEP(*(.initcallearly.init))
		__initcall0_start = .;
		KEEP(*(.initcall0.init))
		KEEP(*(.initcall0s.init))
		__initcall1_start = .;
		KEEP(*(.initcall1.init))
		KEEP(*(.initcall1s.init))
		__initcall2_start = .;
		KEEP(*(.initcall2.init))
		KEEP(*(.initcall2s.init))
		__initcall3_start = .;
		KEEP(*(.initcall3.init))
		KEEP(*(.initcall3s.init))
		__initcall4_start = .;
		KEEP(*(.initcall4.init))
		KEEP(*(.initcall4s.init))
		__initcall5_start = .;
		KEEP(*(.initcall5.init))
		KEEP(*(.initcall5s.init))
		__initcallrootfs_start = .;
		KEEP(*(.initcallrootfs.init))
		KEEP(*(.initcallrootfss.init))
		__initcall6_start = .;
		KEEP(*(.initcall6.init))
		KEEP(*(.initcall6s.init))
		__initcall7_start = .;
		KEEP(*(.initcall7.init))
		KEEP(*(.initcall7s.init))
		__initcall_end = .;
		__con_initcall_start = .;
		KEEP(*(.con_initcall.init))
		__con_initcall_end = .;

		. = ALIGN(4);
		/* init data */
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP(*(SORT(.init_array.*)))
		KEEP(*(.init_array))
		PROVIDE_HIDDEN (__init_array_end = .);


		. = ALIGN(4);
		/* finit data */
		PROVIDE_HIDDEN (__fini_array_start = .);
		KEEP(*(SORT(.fini_array.*)))
		KEEP(*(.fini_array))
		PROVIDE_HIDDEN (__fini_array_end = .);

		KEEP(*(.jcr*))
		. = ALIGN(4);
		/* All data end */
		__data_end__ = .;

	} > RAM
	/*******************************************************************************************/
	/*   CINIT placement                                                                       */
	/*******************************************************************************************/
	.cinit.data :
	{
		. = ALIGN(32);
		__cinit_data__ = .;
		__cinit_start_table_start__ = .;
		KEEP(*(*.cinit_entry_func.data*))
		__cinit_start_table_end__ = .;
		*(.cinit.data*)

		/* put C-init data here */
	} > RAM

	.cinit.ro :
	{
		. = ALIGN(16);
		__cinit_ro_start__ = .;

		/* *libgcc.a:*.o(.text*) */
		*libgcc.a:_arm_addsubdf3.o(.text*)
		*libgcc.a:_arm_muldivdf3.o(.text*)
		*libgcc.a:_arm_fixunsdfsi.o(.text*)
		*(.cinit.text*)
		*(.cinit.rodata*)


		/* put C-init ro-data here */

        __cinit_ro_end__ = .;

	} > RAM


	/*******************************************************************************************/
	/*   XIP placement                                                                         */
	/*******************************************************************************************/

	.xip_reserved :
	{
		/* XIP image section must start with 64K aligned address, reserve 0x140 for image header */
		/*. += 0x140;*/
		
	} > XIP_FLASH


	.xip.code_text :
	{
		. = ALIGN(16);
		__xip_code_text_start__ = .;

		*(.text*)
		
		__xip_code_text_end__ = .;
	} > XIP_FLASH
	
	.ARM.exidx : 
	{ 
		__exidx_start = .; 
		*(.ARM.exidx* .gnu.linkonce.armexidx.*) 
		__exidx_end = .; 
	} > XIP_FLASH		

	.xip.code_rodata :
	{
		. = ALIGN(4);
		__xip_code_rodata_start__ = .;

		*(.rodata*)
		*(.itcm.rodata*)
		*(.haac.rodata*)
		*(.faac.rodata*)
		*(.speex.rodata*)
		
		. = ALIGN(4);
		__xip_code_rodata_end__ = .;
	} > XIP_FLASH

	/* The release placement policy is ITCM/SRAM first and LPDDR otherwise.
	 * Fail at link time if a new ordinary text/rodata section silently falls
	 * back to NOR XIP.  ARM unwind metadata is intentionally left separate. */
	ASSERT(__xip_code_text_end__ == __xip_code_text_start__,
	       "unselected executable text fell back to NOR XIP")
	ASSERT(__xip_code_rodata_end__ == __xip_code_rodata_start__,
	       "unselected read-only data fell back to NOR XIP")

	/*******************************************************************************************/
	/*   Others placement                                                                      */
	/*******************************************************************************************/
	.system_restore_data :
	{
		. = ALIGN(32);
		*(.sys_restore.bss*)
	} > RAM
	
	.ram.bss :
	{
		. = ALIGN(16);
		*(.ram.bss.noinit*)

		__cinit_bss_start__ = .;
		*(.cinit.bss*)
		
		/* put C-init BSS here */
		__cinit_bss_end__ = .;

		__bss_start__ = .;
		/* The 5.5 KiB pointer-message pool is a per-packet data hot spot.  VOE
		 * is disabled in this image, so the pool and its control state can use
		 * shared internal SRAM instead of LPDDR without changing ownership or
		 * queue semantics. */
		*tcpip.o(.eram.bss*)
		*(.bss*)
		*(.sram.bss*)
		*(.itcm.bss*)
		*(.haac.bss*)
		*(.faac.bss*)
		*(.speex.bss*)
		*(COMMON)
	} > RAM

	.non_secure.bss :
	{
		. = ALIGN(16);
		__ns_bss_start__ = .;
		*(.nonsecure.bss*)

		. = ALIGN(4);
		__ns_bss_end__ = .;
		__bss_end__ = .;
	} > RAM

	/* Only 0x20100000..0x2017A000 is qualified as bus-visible internal SRAM
	 * when VOE is disabled.  RAM_LENGTH is intentionally broader for legacy
	 * image layout, so fail the link rather than silently placing static data
	 * beyond the qualified shared-SRAM bank. */
	ASSERT(__bss_end__ <= 0x2017A000,
	       "internal SRAM static data exceeds VOE-disabled safe boundary")

	/* Non-Cacheable memory, the memory range should be configured by a MPU region */
	.non_cacheable_data :
	{
		. = ALIGN(32);
		__nc_mem_start__ = .;
		__nc_data_start__ = .;
		*(.non_cache.data*)
		__nc_data_end__ = .;
	} > RAM_NC

	.non_cacheable_bss :
	{
		. = ALIGN(32);
		__nc_bss_start__ = .;
		*(.non_cache.bss*)
		__nc_bss_end__ = .;
		__nc_mem_end__ = .;
	} > RAM_NC

	/* RAM_SHARED memory is shared by CPU and Video Encoder, CPU can locate memory here only when Video Encoder is off */
	.share_ram :
	{
	} > RAM_SHARED

	.heap (COPY):
	{
		__HeapBase = .;
		__end__ = .;
		end = __end__;
		KEEP(*(.heap*))
		__HeapLimit = .;
	} > DTCM_RAM

	/* .stack_dummy section doesn't contains any symbols. It is only
	 * used for linker to calculate size of stack sections, and assign
	 * values to stack symbols later */
	.stack_dummy (COPY):
	{
		KEEP(*(.stack*))
	} > DTCM_RAM

	/* Set stack top to end of RAM, and stack limit move down by
	 * size of stack_dummy section */
	__StackTop = ORIGIN(DTCM_RAM) + LENGTH(DTCM_RAM);
	__StackLimit = __StackTop - SIZEOF(.stack_dummy);
	PROVIDE(__stack = __StackTop);

	/* Check if data + heap + stack exceeds RAM limit */
/* TODO:	ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed with stack") */
}
