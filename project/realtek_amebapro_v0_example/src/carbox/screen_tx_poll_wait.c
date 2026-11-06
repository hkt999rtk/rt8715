#include "screen_tx_poll_wait.h"

/* v17 自己处理 4KB 分批、USB RTO 和 poll。这里只静默 EAGAIN 日志，
 * 不接管 write、等待、错误返回或发送节奏。 */
int carbox_screen_block_quiet(const char *message)
{
	(void)message;
	return 0;
}
