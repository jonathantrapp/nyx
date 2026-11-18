#pragma once
#include <stdlib.h>
inline bool psramFound() { return false; }
inline void* ps_malloc(size_t bytes) { return malloc(bytes); }
