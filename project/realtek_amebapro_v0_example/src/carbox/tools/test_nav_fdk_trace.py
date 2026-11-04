#!/usr/bin/env python3
"""Check opaque FDK call forwarding and returned state observations."""
from pathlib import Path
import subprocess,tempfile
src=Path(__file__).resolve().parents[1]
h=r'''
#include <assert.h>
#include <stdio.h>
#define CONFIG_AUDIO_NAV_TX_TRACE 1
#include "nav_fdk_trace.c"
static int state=0, calls,seen;
int conceal_state(void *p){assert(p==(void*)1);return state;}
int native_apply(void *c,void *ch,void *st,const void *sr,int n,unsigned char lpd,int ok,unsigned flags){
 assert(c==(void*)1 && ch==(void*)2 && st==(void*)3 && sr==(void*)4);
 assert(n==480 && lpd==0x7f && ok==0 && flags==0x12345678);
 calls++;state=4;return -123;
}
void navtx_fdk_state(int before,int after,int ok,int n){assert(before==0 && after==4 && ok==0 && n==480);seen++;}
int main(void){
 assert(traced_apply((void*)1,(void*)2,(void*)3,(void*)4,480,0x7f,0,0x12345678)==-123);
 assert(calls==1 && seen==1);puts("PASS FDK hook: all native arguments/result unchanged, actual state observed");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'t.c').write_text(h)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-I',str(src),str(p/'t.c'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
