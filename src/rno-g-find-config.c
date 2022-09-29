#include "ice-common.h" 
#include <stdio.h>
#include <unistd.h> 


int main(int nargs, char ** args) 
{
  char * cfgpath = NULL; 
  char * cfgname = "acq.cfg"; 

  if (nargs > 1) cfgname = args[1]; 
  else printf("No config name passed, assuming acq.cfg\n"); 
  if (nargs > 2) 
  {
    cfgpath = args[2]; 
    printf("Using provided cfgpath: %s\n", cfgpath); 
  }

  char * found = 0;
  char * renamed = 0;  
  FILE * f = find_config(cfgname, cfgpath, &found, &renamed); 
  if (!f) 
  {
    printf("Not found!\n"); 
    return 1; 

  }
  printf("Found: %s, renamed? %s\n", found, renamed?: "no"); 
  char link[1024], path[1024]; 
  int fd = fileno(f); 
  sprintf(path,"/proc/self/fd/%d", fd); 
  readlink(path, link, sizeof(link)-1); 
  printf("fd: %d, linkname: %s\n", fd, link); 
  fclose(f); 
  return 0; 




}


