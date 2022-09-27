#ifndef _RNO_G_ICE_COMMON_H
/** Utility functions used by multiple DAQ programs */ 

#include <time.h> 
#include <stdio.h> 

int mkdir_if_needed(const char * path); 

double timespec_difference(const struct timespec *a, const struct timespec* b); 


// Find a config. If cfgpath is not null and a file, we always try to open that file. If it's not found, we check the search path, which consists of
// in order, cfgpath (if not null), CWD, $RNO_G_INSTALL_DIR/cfg and /rno-g/cfg. 
//
// In each element of the search path, we first look for a directory named cfgname.once. If such a directory exists and contains files ending in .cfg, the file ending in .cfg with the earliest ctime will be returned and deleted after opening (consuming it). 
// If there is no cfgname.once directory, then we look for cfgname within that elemnt of the search path (and it is not deleted). Note that a file explicitly passed as cfgpath is not deleted even if it is in the cfgname.once directory.  
// If the search path is exhausted, we return NULL.
//
// If found_path is not NULL, it is set to the found path. New memory will always be allocated, so if you want to call it again, you should free it. 
// 
// If onetime is not null, it is filled with wither or not we are using a onetime file. 
FILE *find_config(const char * cfgname, const char * cfgpath, char ** found_path, int * onetime);

double get_free_MB_by_path(const char * path); 


#endif
