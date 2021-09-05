#define _GNU_SOURCE
#include "ice-common.h" 
#include <stdio.h> 
#include <stdlib.h> 
#include <string.h> 
#include <sys/stat.h> 
#include <sys/file.h> 
#include <sys/statvfs.h>
#include <unistd.h> 



double timespec_difference(const struct timespec * a, const struct timespec * b) 
{

  float sec_diff = a->tv_sec - b->tv_sec; 
  float nanosec_diff = a->tv_nsec - b->tv_nsec; 
  return sec_diff + 1e-9 * (nanosec_diff); 
} 


int mkdir_if_needed(const char * path)
{

  struct stat st; 
  if ( stat(path,&st) || ( (st.st_mode & S_IFMT) != S_IFDIR))//not there, try to make it
  {
     return mkdir(path,0755);
  }

  return 0; 
}



FILE* find_config(const char * cfgname) 
{

  //first try CWD
  if (!access(cfgname, R_OK))
  {
    printf("Using cfg file ./%s\n",cfgname); 
    return fopen(cfgname,"r"); 
  }

  //try RNO_G_INSTALL_DIR/cfg or /rno-g/cfg 
  const char * install_dir = getenv("RNO_G_INSTALL_DIR"); 
  if (install_dir)
  {
    char * fname = 0; 
    asprintf(&fname,"%s/cfg/%s", install_dir,cfgname); 
    if (!access(fname,R_OK))
    {
        printf("Using cfg file %s\n", fname); 
        FILE * fptr = fopen(fname,"r"); 
        free(fname); 
        return fptr; 
    }
    free(fname); 
  }

  char * fname = 0; 
  asprintf(&fname,"/rno-g/cfg/%s", cfgname); 
  if (!access(fname,R_OK))
  {
      FILE * fptr = fopen(fname,"r"); 
      printf("Using cfg file %s\n", fname); 
      free(fname); 
      return fptr; 
  }

  free(fname); 
  fprintf(stderr,"Could not find %s in CWD, $RNO_G_INSTALL_DIR/cfg or /rno-g/cfg\n", cfgname); 
  return 0; 
}

double get_free_MB_by_path(const char * path) 
{
  struct statvfs vfs; 
  statvfs(path,&vfs);
  double MBfree = (((double)vfs.f_bsize) * vfs.f_bavail) / ( (double) (1 << 20)); 
  return MBfree; 
}
