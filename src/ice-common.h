#ifndef _RNO_G_ICE_COMMON_H
/** Utility functions used by multiple DAQ programs */ 

#include <time.h> 

int mkdir_if_needed(const char * path); 

double timespec_difference(const struct timespec *a, const struct timespec* b); 



#endif
