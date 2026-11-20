/* SPDX-License-Identifier: MIT */
#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>
#if defined(REVD_FREERTOS)
#include "rtos/runtime.h"
#include <reent.h>
/* newlib's allocator is global even with per-task _reent. Scheduler suspension
 * is nestable and leaves CAN/SD IRQs enabled; allocator hooks never block. */
void __malloc_lock(struct _reent *r) {
    (void)r;
    configASSERT(__get_IPSR()==0u);
    if (revd_rtos_running()) vTaskSuspendAll();
}
void __malloc_unlock(struct _reent *r) {
    (void)r;
    if (revd_rtos_running()) (void)xTaskResumeAll();
}
#endif
extern char _end, _heap_end;
void *_sbrk(ptrdiff_t n) {static char *cur=&_end;char *old=cur;if(n<0 || cur+n>&_heap_end){errno=ENOMEM;return(void*)-1;}cur+=n;return old;}
int _write(int fd,const void *buf,size_t len){(void)fd;(void)buf;return(int)len;}
int _read(int fd,void *buf,size_t len){(void)fd;(void)buf;(void)len;return 0;}
int _close(int fd){(void)fd;return -1;}
int _fstat(int fd,struct stat*s){(void)fd;s->st_mode=S_IFCHR;return 0;}
int _isatty(int fd){(void)fd;return 1;}
int _lseek(int fd,int o,int w){(void)fd;(void)o;(void)w;return 0;}
int _getpid(void){return 1;}
int _kill(int p,int s){(void)p;(void)s;errno=EINVAL;return -1;}
void _exit(int s){(void)s;for(;;){}}

void _init(void) {}
void _fini(void) {}
