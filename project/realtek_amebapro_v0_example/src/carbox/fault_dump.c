/* Fatal exception diagnostics: no allocator, scheduler, printf or mutex.
 * All code, literals, capture storage and the emergency stack live in SRAM.
 * Reads are bounded to mapped RAM, but cannot recover from failed RAM/UART
 * hardware or a second fault while servicing HardFault.
 */
#include <stddef.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_irq.h"
#include "hal_uart.h"
#include "build_info.h"
#include "fault_dump.h"
#include "fault_dump_frame.h"

extern hal_uart_adapter_t log_uart;
extern void * volatile pxCurrentTCB;

typedef struct {
	uint32_t r4_r11[8];
	uint32_t msp, psp, exc_return, control, primask, basepri, faultmask;
	uint32_t msplim, psplim, exception;
	uint32_t msp_ns, psp_ns, msplim_ns, psplim_ns;
} fault_capture_t;

_Static_assert(offsetof(fault_capture_t, msp) == 32, "assembly capture ABI");
_Static_assert(offsetof(fault_capture_t, psplim_ns) == 84, "assembly capture ABI");
static volatile fault_capture_t fault_capture __attribute__((used));
static volatile uint32_t fault_entered __attribute__((used));
static uint32_t fault_stack[256] __attribute__((used, aligned(8)));
static UART0_Type *fault_uart;
static int fault_uart_failed;

static void fault_putc(char c)
{
	uint32_t budget = 100000U;
	if (fault_uart_failed || !fault_uart) return;
	/* Wait for an empty TX FIFO, then write directly. Interrupt-driven log
	 * draining is stopped by PRIMASK; do not wait for that software queue. */
	while (!(fault_uart->lsr & (1U << 5))) {
		if (!--budget) {
			fault_uart_failed = 1;
			return;
		}
	}
	fault_uart->thr = (uint8_t)c;
}

static void fault_text(const char *s)
{
	while (*s) fault_putc(*s++);
}

static void fault_hex(uint32_t value)
{
	int shift;
	fault_text("0x");
	for (shift = 28; shift >= 0; shift -= 4) {
		unsigned digit = (value >> shift) & 15U;
		fault_putc(digit < 10 ? '0' + digit : 'a' + digit - 10);
	}
}

static void fault_field(const char *name, uint32_t value)
{
	fault_text(name);
	fault_hex(value);
	fault_putc(' ');
}

static void fault_reason(uint32_t status)
{
	fault_text("\r\n[FAULT] causes:");
#define REASON(bit, name) if (status & (1U << (bit))) fault_text(" " name)
	REASON(0, "IACCVIOL"); REASON(1, "DACCVIOL");
	REASON(3, "MUNSTKERR"); REASON(4, "MSTKERR"); REASON(5, "MLSPERR");
	REASON(8, "IBUSERR"); REASON(9, "PRECISERR"); REASON(10, "IMPRECISERR");
	REASON(11, "UNSTKERR"); REASON(12, "STKERR"); REASON(13, "LSPERR");
	REASON(16, "UNDEFINSTR"); REASON(17, "INVSTATE"); REASON(18, "INVPC");
	REASON(19, "NOCP"); REASON(20, "STKOF");
	REASON(24, "UNALIGNED"); REASON(25, "DIVBYZERO");
#undef REASON
	fault_text("\r\n");
}

static void fault_stack_words(uint32_t sp)
{
	unsigned i, j;
	fault_text("[FAULT] stack words (raw data, not an unwound backtrace):\r\n");
	for (i = 0; i < 64U; i += 4U) {
		if (!carbox_fault_ram(sp, 16U)) {
			fault_text("[FAULT] stack stopped at RAM boundary\r\n");
			break;
		}
		fault_hex(sp);
		fault_text(": ");
		for (j = 0; j < 4; ++j) {
			fault_hex(((volatile uint32_t *)(uintptr_t)sp)[j]);
			fault_putc(' ');
		}
		fault_text("\r\n");
		sp += 16U;
	}
}

__attribute__((used, noreturn, noinline))
static void fault_dump_run(void)
{
	uint32_t cfsr = SCB->CFSR, hfsr = SCB->HFSR;
	uint32_t exc = fault_capture.exc_return;
	uint32_t sp = (exc & 4U) ? fault_capture.psp : fault_capture.msp;
	uint32_t limit = (exc & 4U) ? fault_capture.psplim : fault_capture.msplim;
	uint32_t frame = 0, frame_status = cfsr;
	unsigned i;
	static const char * const names[] = {
		"r0=", "r1=", "r2=", "r3=", "r12=", "lr=", "pc=", "xpsr="
	};

	fault_text("\r\n[FAULT] BEGIN build=" BOX_APP_VERSION " type=");
	switch (fault_capture.exception) {
	case 3: fault_text("HardFault"); break;
	case 4: fault_text("MemManage"); break;
	case 5: fault_text("BusFault"); break;
	case 6: fault_text("UsageFault"); break;
	default: fault_text("unknown"); break;
	}
	fault_text("\r\n[FAULT] ");
	fault_field("cfsr=", cfsr); fault_field("ufsr=", cfsr >> 16);
	fault_field("hfsr=", hfsr); fault_field("shcsr=", SCB->SHCSR);
	fault_text("\r\n[FAULT] ");
	fault_field("icsr=", SCB->ICSR); fault_field("dfsr=", SCB->DFSR);
	fault_field("afsr=", SCB->AFSR);
	if (cfsr & (1U << 7)) fault_field("mmfar=", SCB->MMFAR);
	if (cfsr & (1U << 15)) fault_field("bfar=", SCB->BFAR);
	fault_reason(cfsr);
	fault_text("[FAULT] ");
	fault_field("exc_return=", exc); fault_field("control=", fault_capture.control);
	fault_field("primask=", fault_capture.primask);
	fault_field("basepri=", fault_capture.basepri);
	fault_field("faultmask=", fault_capture.faultmask);
	fault_text("\r\n[FAULT] ");
	fault_field("msp=", fault_capture.msp); fault_field("msplim=", fault_capture.msplim);
	fault_field("psp=", fault_capture.psp); fault_field("psplim=", fault_capture.psplim);
	fault_text("\r\n[FAULT] ");
	for (i = 0; i < 8; ++i) {
		fault_putc('r');
		if (i + 4 >= 10) fault_putc('1');
		fault_putc('0' + (i + 4) % 10);
		fault_putc('='); fault_hex(fault_capture.r4_r11[i]); fault_putc(' ');
		if (i == 3) fault_text("\r\n[FAULT] ");
	}
	fault_text("\r\n");
	if (!(exc & (1U << 6))) {
		sp = (exc & 4U) ? fault_capture.psp_ns : fault_capture.msp_ns;
		limit = (exc & 4U) ? fault_capture.psplim_ns : fault_capture.msplim_ns;
		frame_status |= SCB_NS->CFSR;
		fault_text("[FAULT] nonsecure stack "); fault_field("cfsr_ns=", SCB_NS->CFSR);
		fault_text("\r\n");
	}
	fault_text("[FAULT] "); fault_field("frame_sp=", sp);
	fault_field("frame_limit=", limit); fault_text("\r\n");
	if (carbox_fault_core_frame(sp, exc, frame_status, limit, &frame)) {
		volatile uint32_t *words = (volatile uint32_t *)(uintptr_t)frame;
		fault_text("[FAULT] ");
		for (i = 0; i < 8; ++i) {
			fault_field(names[i], words[i]);
			if (i == 3) fault_text("\r\n[FAULT] ");
		}
		fault_text("\r\n");
		fault_text("[FAULT] ");
		fault_field("pre_exception_sp=", sp + ((exc & 16U) ? 32U : 104U) +
			((words[7] & (1U << 9)) ? 4U : 0U));
		fault_text("\r\n");
		fault_stack_words(sp);
	} else {
		fault_text("[FAULT] frame unavailable: stacking fault, invalid SP/limit, "
			   "or unsupported EXC_RETURN/additional-state frame\r\n");
	}
	/* TCB address is useful with the matching ELF without dereferencing a
	 * potentially corrupt TCB or invoking RTOS list/heap/stack traversal. */
	fault_text("[FAULT] "); fault_field("current_tcb=", (uint32_t)pxCurrentTCB);
	fault_text("\r\n[FAULT] END; halted (watchdog may reset)\r\n");
	for (;;) __asm volatile("nop");
}

/* Capture before any compiler prologue; never push onto the damaged stack.
 * R0-R3/R12/LR/PC/xPSR are read from the hardware frame, R4-R11 here. */
__attribute__((naked, used, noreturn))
static void fault_entry(void)
{
	__asm volatile(
		"mrs r1, primask\n"
		"cpsid i\n"
		"ldr r0, =fault_entered\n"
		"ldr r2, [r0]\n"
		"cmp r2, #0\n"
		"bne 1f\n"
		"movs r2, #1\n"
		"str r2, [r0]\n"
		"ldr r0, =fault_capture\n"
		"stmia r0, {r4-r11}\n"
		"str r1, [r0, #48]\n"
		"mrs r1, msp\n"     "str r1, [r0, #32]\n"
		"mrs r1, psp\n"     "str r1, [r0, #36]\n"
		"str lr, [r0, #40]\n"
		"mrs r1, control\n" "str r1, [r0, #44]\n"
		"mrs r1, basepri\n" "str r1, [r0, #52]\n"
		"mrs r1, faultmask\n" "str r1, [r0, #56]\n"
		"mrs r1, msplim\n"  "str r1, [r0, #60]\n"
		"mrs r1, psplim\n"  "str r1, [r0, #64]\n"
		"mrs r1, ipsr\n"    "str r1, [r0, #68]\n"
		"mrs r1, msp_ns\n"  "str r1, [r0, #72]\n"
		"mrs r1, psp_ns\n"  "str r1, [r0, #76]\n"
		"mrs r1, msplim_ns\n" "str r1, [r0, #80]\n"
		"mrs r1, psplim_ns\n" "str r1, [r0, #84]\n"
		"movs r1, #0\n"
		"msr msplim, r1\n"
		"ldr r1, =fault_stack\n"
		"add r2, r1, #1024\n"
		"msr msp, r2\n"
		"msr msplim, r1\n"
		"isb\n"
		"b fault_dump_run\n"
		"1: b 1b\n");
}

int carbox_fault_dump_init(void)
{
	uint32_t mask, vtor = SCB->VTOR;
	volatile uint32_t *vectors = (volatile uint32_t *)(uintptr_t)vtor;
	/* The ROM normally installs its vector table in this DTCM reservation. */
	if (!((vtor >= 0x20000000U && vtor <= 0x20010000U - 64U) ||
	      (vtor >= 0x20100a00U && vtor <= 0x20180000U - 64U))) return -1;
	fault_uart = log_uart.base_addr;
	if (!fault_uart) return -2;
	mask = __get_PRIMASK();
	__disable_irq();
	vectors[3] = (uint32_t)fault_entry;
	vectors[4] = (uint32_t)fault_entry;
	vectors[5] = (uint32_t)fault_entry;
	vectors[6] = (uint32_t)fault_entry;
	__DSB(); __ISB();
	__set_PRIMASK(mask);
	/* Leave fault-enable/trap policy unchanged; disabled faults escalate to
	 * HardFault and are decoded there using the same CFSR. */
	return 0;
}
