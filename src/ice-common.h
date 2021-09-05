#ifndef _RNO_G_ICE_COMMON_H
/** Utility functions used by multiple DAQ programs */ 

#include <time.h> 
#include <stdio.h> 

int mkdir_if_needed(const char * path); 

double timespec_difference(const struct timespec *a, const struct timespec* b); 

FILE *find_config(const char * cfgname); //e.g. cfgname="acq.cfg" 

double get_free_MB_by_path(const char * path); 


#endif
