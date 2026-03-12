#pragma once

extern void* find_symbol_in_lib(const char* lib_pattern, const char* symbol_name);

// RTLD_NEXT emulation: find `symbol_name` in the first library loaded AFTER
// the one that contains `caller_addr`.
extern void* find_symbol_next(void* caller_addr, const char* symbol_name);
