#ifndef CARBOX_FAULT_DUMP_H
#define CARBOX_FAULT_DUMP_H

/* Install after the log UART and RAM vector table are initialized. */
int carbox_fault_dump_init(void);

#endif
