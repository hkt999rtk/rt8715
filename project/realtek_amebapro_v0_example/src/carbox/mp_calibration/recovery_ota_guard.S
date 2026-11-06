/* 只链接到FW2。闭源OTA入口的透明跳板，不猜测其C函数参数原型。
 * ota_httpd.o/sta_ota.o对ota_update_init是外部引用，由--wrap重定向。
 * 保存被使用的参数寄存器，保持栈8字节对齐；r2/r3及所有栈上传参不变。
 * 恢复栈后尾调用原函数，原返回值/LR均不改变。标记只置位，复位时清零。
 * 即使OTA初始化失败，本次启动也不再进入MP，防止使用旧槽位信息。
 */
.syntax unified
.thumb
.section .text.__wrap_ota_update_init,"ax",%progbits
.global __wrap_ota_update_init
.type __wrap_ota_update_init,%function
.thumb_func
__wrap_ota_update_init:
    push {r0, r1}
    ldr r0, =recovery_ota_started
    movs r1, #1
    str r1, [r0]
    pop {r0, r1}
    b.w __real_ota_update_init
.size __wrap_ota_update_init, .-__wrap_ota_update_init
