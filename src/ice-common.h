#ifndef _RNO_G_ICE_COMMON_H
/** Utility functions used by multiple DAQ programs */ 

#include <time.h> 
#include <stdio.h> 

int mkdir_if_needed(const char * path); 

double timespec_difference(const struct timespec *a, const struct timespec* b); 

// Find a config. If cfgpath is not null and a file, we try opening that. If it's a dir,we look for cfgname inside cfgpath. 
// If cfgpath is NULL, we search CWD, followed by $RNO_G_INSTALL_DIR/cfg followed by /rno-g/cfg for cfgname. 
// If found_path is not NULL, it is set to the found path! New memory will always be allocated, so if you want to call it again, you should free it. 
FILE *find_config(const char * cfgname, const char * cfgpath, char ** found_path);

double get_free_MB_by_path(const char * path); 


#endif
