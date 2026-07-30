/*
 * test_led.c — LP 核心 RGB LED PWM 测试
 *
 * 硬件:
 *   RTL8195B LP core, 三路 PWM 驱动 RGB LED
 *
 * 引脚:
 *   LED_R → PA_6  (PWM6_S0)
 *   LED_G → PA_13 (PWM7_S0)
 *   LED_B → PA_4  (PWM4_S0)
 *
 * 编译要求:
 *   在 LP 镜像编译中定义 CONFIG_PWM_EN=1
 *
 * 效果:
 *   1. 彩虹渐变色轮循环
 *   2. R/G/B 逐色呼吸
 *   3. 单色渐变
 *
 * 共阳 LED 适配:
 *   定义 RGB_LED_ACTIVE_LOW 后占空比自动反转
 */

#include "device.h"
#include "pwmout_api.h"

/* ---- 引脚定义 ---- */
#define LED_R_PIN  PA_6
#define LED_G_PIN  PA_13
#define LED_B_PIN  PA_4

/* ---- PWM 参数 ---- */
#define PWM_PERIOD_US   1000    /* 1kHz PWM 频率，LED 无闪烁 */

/* ---- 渐变步数（越大越平滑但越慢） ---- */
#define FADE_STEPS      256
#define FADE_DELAY_LOOPS  400    /* NOP 循环数，调延时 */

/* ---- 共阳 LED 适配 ---- */
/* #define RGB_LED_ACTIVE_LOW  1 */

/* ---- 全局 ---- */
static pwmout_t pwm_r, pwm_g, pwm_b;

/* ---- 延时（NOP 忙等） ---- */
static void delay_loop(int loops)
{
	volatile int i;
	for (i = 0; i < loops; i++) {
		__NOP();
	}
}

/* ---- 设置单通道亮度 ---- */
static inline void led_set(pwmout_t *ch, float brightness)
{
#ifdef RGB_LED_ACTIVE_LOW
	pwmout_write(ch, 1.0f - brightness);
#else
	pwmout_write(ch, brightness);
#endif
}

static inline void led_r(float v) { led_set(&pwm_r, v); }
static inline void led_g(float v) { led_set(&pwm_g, v); }
static inline void led_b(float v) { led_set(&pwm_b, v); }

/* ---- 全灭 ---- */
static void led_off(void)
{
	led_r(0.0f);
	led_g(0.0f);
	led_b(0.0f);
}

/* ---- 全亮白 ---- */
static void led_white(void)
{
	led_r(1.0f);
	led_g(1.0f);
	led_b(1.0f);
}

/*
 * 将 HSV 色相转换为 RGB 并输出
 *   h: 0.0 - 360.0 色相角
 *   s: 0.0 - 1.0   饱和度（固定 1.0）
 *   v: 0.0 - 1.0   明度
 */
static void led_hsv(float h, float s, float v)
{
	float r, g, b;
	int   sector;
	float f, p, q, t;

	if (s <= 0.0f) {
		r = g = b = v;
	} else {
		h /= 60.0f;
		sector = (int)h;
		f = h - (float)sector;
		p = v * (1.0f - s);
		q = v * (1.0f - s * f);
		t = v * (1.0f - s * (1.0f - f));

		switch (sector) {
		case 0:  r = v; g = t; b = p; break;
		case 1:  r = q; g = v; b = p; break;
		case 2:  r = p; g = v; b = t; break;
		case 3:  r = p; g = q; b = v; break;
		case 4:  r = t; g = p; b = v; break;
		default: r = v; g = p; b = q; break;
		}
	}

	led_r(r);
	led_g(g);
	led_b(b);
}

/* ---- 效果 1: 彩虹色轮 ---- */
static void effect_rainbow(int cycles)
{
	int i;

	for (i = 0; i < cycles * FADE_STEPS; i++) {
		float hue = (float)(i % FADE_STEPS) / (float)FADE_STEPS * 360.0f;
		led_hsv(hue, 1.0f, 0.8f);
		delay_loop(FADE_DELAY_LOOPS);
	}
}

/* ---- 效果 2: 单色呼吸 ---- */
static void effect_breathe(pwmout_t *ch, int cycles)
{
	int i;

	for (i = 0; i < cycles; i++) {
		int step;

		/* 渐亮 */
		for (step = 0; step <= FADE_STEPS; step++) {
			led_set(ch, (float)step / (float)FADE_STEPS);
			delay_loop(FADE_DELAY_LOOPS / 2);
		}
		/* 渐暗 */
		for (step = FADE_STEPS; step >= 0; step--) {
			led_set(ch, (float)step / (float)FADE_STEPS);
			delay_loop(FADE_DELAY_LOOPS / 2);
		}
	}
}

/* ---- RGB 同步呼吸 ---- */
static void effect_breathe_white(int cycles)
{
	int i;

	for (i = 0; i < cycles; i++) {
		int step;
		float v;

		for (step = 0; step <= FADE_STEPS; step++) {
			v = (float)step / (float)FADE_STEPS;
			led_r(v);
			led_g(v);
			led_b(v);
			delay_loop(FADE_DELAY_LOOPS / 3);
		}
		for (step = FADE_STEPS; step >= 0; step--) {
			v = (float)step / (float)FADE_STEPS;
			led_r(v);
			led_g(v);
			led_b(v);
			delay_loop(FADE_DELAY_LOOPS / 3);
		}
	}
}

/* ---- 入口 ---- */
void led_test(void)
{
	/* 初始化三路 PWM */
	pwmout_init(&pwm_r, LED_R_PIN);
	pwmout_init(&pwm_g, LED_G_PIN);
	pwmout_init(&pwm_b, LED_B_PIN);

	/* 统一设置 PWM 周期 */
	pwmout_period_us(&pwm_r, PWM_PERIOD_US);
	pwmout_period_us(&pwm_g, PWM_PERIOD_US);
	pwmout_period_us(&pwm_b, PWM_PERIOD_US);

	led_off();

	while (1) {
		/* 1. 彩虹色轮 (2 圈) */
		effect_rainbow(2);

		/* 2. R/G/B 逐色呼吸 */
		effect_breathe(&pwm_r, 2);
		led_off();
		delay_loop(FADE_DELAY_LOOPS * 50);

		effect_breathe(&pwm_g, 2);
		led_off();
		delay_loop(FADE_DELAY_LOOPS * 50);

		effect_breathe(&pwm_b, 2);
		led_off();
		delay_loop(FADE_DELAY_LOOPS * 50);

		/* 3. 白光呼吸 */
		effect_breathe_white(2);
	}
}
