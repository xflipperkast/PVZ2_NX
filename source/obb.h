/* obb.h -- PVZ2 1BSR/PGSR archive reader interface. */

#ifndef __OBB_H__
#define __OBB_H__

#include <stddef.h>

int   obb_open(const char *path);
void  obb_close(void);
int   obb_exists(const char *name);
void *obb_read(const char *name, size_t *out_size);
void  obb_startup_profile_snapshot(const char *tag);

#endif
