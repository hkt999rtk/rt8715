#ifndef LED_RGB_TEST_MOCK_H
#define LED_RGB_TEST_MOCK_H
#include <stdint.h>
typedef int hal_status_t;
typedef int PinName;
typedef struct { int unused; } hal_icc_adapter_t;
typedef void (*icc_user_cmd_handler_t)(uint32_t, uint32_t, uint32_t);
typedef struct { int pin; float duty; } pwmout_t;
typedef struct { int pin; int level; int output; } gpio_t;
enum { HAL_OK, HAL_BUSY, HAL_TIMEOUT, HAL_NOT_READY };
enum { PA_6 = 6, PA_13 = 13, PA_4 = 4, taskSCHEDULER_RUNNING = 2 };
enum { PullUp = 1, PullDown = 2, PIN_OUTPUT = 1 };
unsigned __get_IPSR(void);
unsigned __get_PRIMASK(void);
unsigned __get_BASEPRI(void);
void __disable_irq(void);
void __set_PRIMASK(unsigned);
#define __DMB() ((void)0)
#define dbg_printf rt_printf
void mock_enter(void);
void mock_exit(void);
#define taskENTER_CRITICAL() mock_enter()
#define taskEXIT_CRITICAL() mock_exit()
int xTaskGetSchedulerState(void);
void vTaskDelay(unsigned);
uint32_t hal_read_curtime_us(void);
hal_status_t hal_icc_hal_cmd_register(hal_icc_adapter_t *, uint8_t,
                                     icc_user_cmd_handler_t, uint32_t);
hal_status_t hal_icc_h2l_cmd_send(hal_icc_adapter_t *, uint32_t, uint32_t, uint32_t);
hal_status_t hal_icc_l2h_cmd_send(hal_icc_adapter_t *, uint32_t, uint32_t, uint32_t);
void pwmout_init(pwmout_t *, int);
void pwmout_free(pwmout_t *);
int pwmout_init_with_duty(pwmout_t *, PinName, int, float);
void pwmout_period_us(pwmout_t *, int);
void pwmout_write(pwmout_t *, float);
void gpio_init(gpio_t *, PinName);
void gpio_mode(gpio_t *, int);
void gpio_write(gpio_t *, int);
void gpio_dir(gpio_t *, int);
void gpio_deinit(gpio_t *);
#endif
