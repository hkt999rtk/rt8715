#ifndef CARBOX_SCREEN_SESSION_H
#define CARBOX_SCREEN_SESSION_H
#include <stddef.h>
#include <stdint.h>
#include "lwip/sockets.h"
void AirPlayScreen_StartVideo(int width,int height);
void AirPlayScreen_StopVideo(void);
void AirPlayScreen_SendScreenData(char *data,int length);
void AirPlayScreen_SendVideo(uint8_t *data,int length);
int carbox_screen_write(int fd,const void *data,size_t length);
int carbox_screen_poll(struct pollfd *fds,unsigned count,int timeout);
int carbox_screen_detach(void *attr,int state);
int carbox_screen_create(void **thread,const void *attr,void *(*entry)(void*),void *arg);
void carbox_screen_thread_exit(void *result);
#endif
