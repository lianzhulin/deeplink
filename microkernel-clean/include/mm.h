
#ifndef MK_MM_H
#define MK_MM_H

#include "kernel.h"
#include "ipc.h"

#define MK_MM_TAG_ALLOC   0x4D41    
#define MK_MM_TAG_FREE    0x4D46    
#define MK_MM_TAG_QUERY   0x4D51    

void mk_mm_start(void);

void *mk_alloc(size_t size);

void  mk_free(void *ptr);

mk_err_t mk_mm_query(int32_t out_buf[3]);

#endif 
