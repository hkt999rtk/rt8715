#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include "../../led_rgb_hp.c"
#include "../../led_rgb_lp.c"

hal_icc_adapter_t icc_hal_adp;
static icc_user_cmd_handler_t request_cb, ack_cb;
static unsigned irq, primask, basepri, critical_depth, sends, writes;
static uint32_t time_us;
static bool missing_ack, corrupt_ack, drop_request, reenter;
static int register_status = HAL_OK, send_status = HAL_OK, ack_busy;
unsigned __get_IPSR() { return irq; }
unsigned __get_PRIMASK() { return primask; }
unsigned __get_BASEPRI() { return basepri; }
void __disable_irq() { primask = 1; }
void __set_PRIMASK(unsigned value) { primask = value; }
void mock_enter() { ++critical_depth; }
void mock_exit() { assert(critical_depth); --critical_depth; }
int xTaskGetSchedulerState() { return taskSCHEDULER_RUNNING; }
uint32_t hal_read_curtime_us() { return time_us; }
void vTaskDelay(unsigned ticks) { time_us += ticks * 1000; led_rgb_lp_poll(); }
void icc_hal_app_init() {}
hal_status_t hal_icc_hal_cmd_register(hal_icc_adapter_t *, uint8_t id,
                                     icc_user_cmd_handler_t cb, uint32_t) {
    if (register_status != HAL_OK) return register_status;
    if (id == LED_RGB_ICC_SET) request_cb = cb;
    else { assert(id == LED_RGB_ICC_ACK); ack_cb = cb; }
    return HAL_OK;
}
hal_status_t hal_icc_h2l_cmd_send(hal_icc_adapter_t *, uint32_t cmd,
                                  uint32_t payload, uint32_t timeout) {
    assert(!critical_depth && timeout == 1000);
    ++sends;
    if (reenter) {
        reenter = false;
        led_rgb(0, 0, 0);
        assert(led_rgb_get_status() == LED_RGB_BUSY);
    }
    if (send_status != HAL_OK) return send_status;
    if (!drop_request) {
        unsigned before = writes;
        irq = 1; request_cb(cmd, payload, 0); irq = 0;
        assert(writes == before); // Never touch PWM from the ICC IRQ.
    }
    return HAL_OK;
}
hal_status_t hal_icc_l2h_cmd_send(hal_icc_adapter_t *, uint32_t cmd,
                                  uint32_t payload, uint32_t timeout) {
    assert(!irq && timeout == 0);
    if (ack_busy) { --ack_busy; return HAL_BUSY; }
    if (!missing_ack) {
        irq = 1;
        ack_cb(corrupt_ack ? (cmd ^ 1U) : cmd, payload, 0);
        irq = 0;
    }
    return HAL_OK;
}
void pwmout_init(pwmout_t *pwm, int pin) { pwm->pin = pin; }
void pwmout_period_us(pwmout_t *, int period) { assert(period == 1000); }
void pwmout_write(pwmout_t *pwm, float value) {
    assert(!irq && value >= 0 && value <= 1);
    pwm->duty = value; ++writes;
}
static void expect_duty(float r, float g, float b) {
#if RGB_LED_ACTIVE_LOW
    r = 1-r; g = 1-g; b = 1-b;
#endif
    assert(std::fabs(red.duty-r) < 0.00001f);
    assert(std::fabs(green.duty-g) < 0.00001f);
    assert(std::fabs(blue.duty-b) < 0.00001f);
}
int main() {
    assert(led_rgb_get_status() == LED_RGB_NOT_READY);
    assert(led_rgb_lp_init() == 0);
    expect_duty(0,0,0);
    register_status = HAL_NOT_READY;
    led_rgb(1,0,0); assert(led_rgb_get_status() == LED_RGB_NOT_READY);
    register_status = HAL_OK;
    led_rgb(1,0.5f,0); assert(led_rgb_get_status() == LED_RGB_OK);
    expect_duty(1,128.0f/255,0);
    led_rgb(0,0,0); expect_duty(0,0,0);
    unsigned before = sends;
    for (float bad : {-0.1f, 1.1f, NAN, INFINITY}) {
        led_rgb(bad,0,0); assert(led_rgb_get_status() == LED_RGB_INVALID_ARGUMENT);
        led_rgb(0,bad,0); assert(led_rgb_get_status() == LED_RGB_INVALID_ARGUMENT);
        led_rgb(0,0,bad); assert(led_rgb_get_status() == LED_RGB_INVALID_ARGUMENT);
    }
    assert(sends == before);
    irq = 1; led_rgb(1,0,0); assert(led_rgb_get_status() == LED_RGB_INVALID_CONTEXT); irq = 0;
    primask = 1; led_rgb(1,0,0); assert(led_rgb_get_status() == LED_RGB_INVALID_CONTEXT); primask = 0;
    basepri = 1; led_rgb(1,0,0); assert(led_rgb_get_status() == LED_RGB_INVALID_CONTEXT); basepri = 0;
    send_status = HAL_BUSY; led_rgb(1,0,0); assert(led_rgb_get_status() == LED_RGB_BUSY);
    send_status = HAL_TIMEOUT; led_rgb(1,0,0); assert(led_rgb_get_status() == LED_RGB_TIMEOUT);
    send_status = HAL_OK;
    missing_ack = true; led_rgb(0,1,0); assert(led_rgb_get_status() == LED_RGB_TIMEOUT);
    missing_ack = false;
    corrupt_ack = true; led_rgb(0,0,1); assert(led_rgb_get_status() == LED_RGB_TIMEOUT);
    corrupt_ack = false;
    drop_request = true; led_rgb(1,1,1); assert(led_rgb_get_status() == LED_RGB_TIMEOUT);
    drop_request = false;
    reenter = true; ack_busy = 3; time_us = UINT32_MAX - 1000;
    led_rgb(1,1,1); assert(led_rgb_get_status() == LED_RGB_OK); expect_duty(1,1,1);
    // Duplicate requests (e.g. after an HP restart) must still receive ACK.
    sequence = applied_sequence - 1;
    led_rgb(1,1,1); assert(led_rgb_get_status() == LED_RGB_OK);
    before = writes;
    request_cb(LED_RGB_ICC_SET << 24, LED_RGB_VERSION, 0);
    request_cb((LED_RGB_ICC_SET << 24)|1, 0x02000000, 0);
    led_rgb_lp_poll(); assert(writes == before);
    assert(!critical_depth);
    puts("LED HP/LP mocked protocol tests passed (not hardware validation)");
}
