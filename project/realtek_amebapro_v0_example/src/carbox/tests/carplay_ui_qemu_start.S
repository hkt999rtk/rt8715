.syntax unified
.thumb
.global _start
.type _start,%function
_start:
 ldr r0,[sp]
 add r1,sp,#4
 bl main
 bl _exit
