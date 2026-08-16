/* imports.h -- native-library import resolution interface */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdio.h>
#include <stdlib.h>
#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;

void update_imports(void);
void imports_startup_profile_snapshot(const char *tag);

void nimble_cpp_component_register(const void *name_object,
                                   const void *shared_ptr_object);
void nimble_cpp_component_get(void);
extern uintptr_t nimble_cpp_component_native_get;
extern uintptr_t nimble_cpp_component_native_register;
void nimble_cpp_component_native_lookup(void *result_shared_ptr,
                                        const void *name_object);
typedef struct Pvz2AllocTraceInfo {
  uintptr_t base;
  size_t size;
  uintptr_t caller;
  uintptr_t parent1;
  uintptr_t parent2;
  uintptr_t parent3;
  unsigned sequence;
  unsigned kind;
} Pvz2AllocTraceInfo;

int pvz2_alloc_trace_lookup(const void *address, Pvz2AllocTraceInfo *out);

#endif
