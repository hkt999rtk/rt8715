/*
 * test_serial.c — UART2 串口功能测试
 *
 * 硬件配置:
 *   UART2 TX → PC_9  (PinSel0)
 *   UART2 RX → PC_8  (PinSel0)
 *
 * 测试内容:
 *   1. 基础初始化与单字节发送
 *   2. 阻塞式字符串发送
 *   3. 阻塞式接收回显（外接 USB-TTL 时可用）
 */

#include "serial_api.h"
#include "diag.h"

/* ---- 硬件引脚定义 ---- */
#define UART2_TX    PC_9
#define UART2_RX    PC_8

/* ---- 全局变量 ---- */
static serial_t uart2;

/* ---- 内部辅助 ---- */

/**
 * @brief  阻塞式发送字符串
 * @param  str: 以 '\0' 结尾的字符串
 * @note   内部调用 serial_putc，逐字节发送
 */
static void uart2_puts(const char *str)
{
	while (*str) {
		serial_putc(&uart2, (int)*str);
		str++;
	}
}

/**
 * @brief  阻塞式接收一行（直到 '\n' 或 '\r'）
 * @param  buf:   接收缓冲区
 * @param  maxlen: 缓冲区大小（含 '\0'）
 * @return 实际接收到的字节数（不含 '\0'）
 */
static int uart2_gets(char *buf, int maxlen)
{
	int i = 0;
	int c;

	while (i < (maxlen - 1)) {
		c = serial_getc(&uart2);

		/* 回显 */
		serial_putc(&uart2, (char)c);

		if (c == '\r' || c == '\n') {
			serial_putc(&uart2, '\r');
			serial_putc(&uart2, '\n');
			break;
		}

		buf[i++] = (char)c;
	}

	buf[i] = '\0';
	return i;
}

/* ---- UART2 初始化 ---- */

void uart2_init(void)
{
	serial_init(&uart2, UART2_TX, UART2_RX);
	serial_baud(&uart2, 115200);
	serial_format(&uart2, 8, ParityNone, 1);
}

/* ---- 测试函数 ---- */

/**
 * @brief  测试1: 基础 TX — 发送 "OK\r\n"
 */
void test_serial_tx_basic(void)
{
	serial_putc(&uart2, 'O');
	serial_putc(&uart2, 'K');
	serial_putc(&uart2, '\r');
	serial_putc(&uart2, '\n');
}

/**
 * @brief  测试2: 字符串发送 — 发送 banner
 */
void test_serial_tx_banner(void)
{
	uart2_puts("\r\n====================================\r\n");
	uart2_puts("  UART2 Serial Test (TX)\r\n");
	uart2_puts("  Baudrate: 115200, 8N1\r\n");
	uart2_puts("====================================\r\n");
}

/**
 * @brief  测试3: 回环/回显测试 — 接收字符并回显
 * @note   需要外部将 UART2 TX 接 RX 或通过 USB-TTL 发送数据
 */
void test_serial_echo(void)
{
	int c;

	uart2_puts("\r\n--- Echo Test (press 'q' to quit) ---\r\n");

	for (;;) {
		c = serial_getc(&uart2);
		serial_putc(&uart2, (char)c);

		if (c == 'q') {
			uart2_puts("\r\n[echo] quit\r\n");
			break;
		}
	}
}

/**
 * @brief  测试4: 阻塞式收发 — 发送提示并等待一行输入
 */
void test_serial_rx_line(void)
{
	char buf[64];
	int len;

	uart2_puts("\r\nEnter your name: ");

	len = uart2_gets(buf, sizeof(buf));

	uart2_puts("Hello, ");
	uart2_puts(buf);
	uart2_puts("!\r\n");
	uart2_puts("Received ");

	/* simple itoa for small positive int */
	{
		char num[8];
		int pos = sizeof(num) - 1;
		int v = len;

		num[pos--] = '\0';
		if (v == 0) {
			num[pos--] = '0';
		} else {
			while (v > 0 && pos >= 0) {
				num[pos--] = (char)('0' + (v % 10));
				v /= 10;
			}
		}
		uart2_puts(&num[pos + 1]);
	}

	uart2_puts(" bytes\r\n");
}

/**
 * @brief  UART2 综合测试入口
 *
 * 按顺序执行:
 *   1. 初始化 UART2
 *   2. TX 基础测试
 *   3. TX banner 测试
 *   4. 回环测试 (交互式)
 *   5. 接收测试  (交互式)
 */
void uart2_test(void)
{
	rt_printf("[uart2_test] init UART2 (PC_9=TX, PC_8=RX, 115200-8N1)\r\n");
	uart2_init();
	rt_printf("[uart2_test] init done, sending TX test...\r\n");

	/* --- 非交互测试 --- */
	test_serial_tx_basic();
	test_serial_tx_banner();

	/*
	 * 交互测试（需外接串口终端，默认禁用避免阻塞）
	 * 需要时取消注释:
	 *   test_serial_echo();
	 *   test_serial_rx_line();
	 */

	uart2_puts("\r\n=== UART2 TX test done ===\r\n");
}
