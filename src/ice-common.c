#define _GNU_SOURCE
#include "ice-common.h" 
#include <stdio.h> 
#include <stdlib.h> 
#include <string.h> 
#include <sys/stat.h> 
#include <sys/file.h> 
#include <sys/statvfs.h>
#include <unistd.h> 
#include <dirent.h>
#include <limits.h> 


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
static int cfgfilter(const struct dirent * d)
{
  char * dot = strrchr(d->d_name,'.'); 
  if (!strcasecmp(dot,".cfg"))
  {
    return 1; 
  }
  return 0; 
}


static FILE * check_file(const char * fname) 
{
  FILE * f = fopen(fname,"r"); 
  if (!f) return NULL; 

  int fd = fileno(f); 
  struct stat st; 
  fstat(fd,&st); 

  //NOT a regular file! 
  if (!S_ISREG(st.st_mode))
  {
    fclose(f); 
    return NULL; 
  }

  return f;

}
static FILE * check_dir(const char * dirname, const char * cfgname, char ** found_path, int * onetime)
{
  //first check we have a dir
  struct stat st; 

  if (stat(dirname,&st))
      return NULL; //not found

  //not a dir
  if (!S_ISDIR(st.st_mode)) return NULL; 

  //check for dirname/cfgname.once dir
  char * fname = 0; 
  asprintf(&fname,"%s/%s.once", dirname, cfgname); 


  //is the once directory a directory? 
  if (!stat(fname,&st) && S_ISDIR(st.st_mode))
  {
    struct dirent **name_list; 
    int n = scandir(fname, &name_list, cfgfilter,0); 

    //now find which has the earliest ctime
    int earliest_i = -1; 
    double min_ctime = INT_MAX; 
    int dirfd = open(fname,O_PATH); 
    for (int i = 0; i < n; i++) 
    {
      
      if (!fstatat(dirfd, name_list[i]->d_name,&st,0) &&  
          S_ISREG(st.st_mode) && 
          st.st_ctim.tv_sec  + st.st_ctim.tv_nsec*1e-9 < min_ctime)
      {
        earliest_i = i; 
        min_ctime = st.st_ctim.tv_sec + 1e-9 * st.st_ctim.tv_nsec; 
      }
    }
    close(dirfd); 

    if (earliest_i >= 0)
    {
      free(fname); 
      asprintf(&fname,"%s/%s.once/%s", dirname, cfgname,name_list[earliest_i]->d_name); 
      free(name_list); 

      FILE * f = check_file(fname); 
      if (f) 
      {
        if (found_path) *found_path = fname; 
        else free(fname); 

        if (onetime) *onetime = 1; 
        unlink(fname); 
        return f; 
      }
      else
      {
        fprintf(stderr,"Uh oh, was %s deleted from underneath us? Recursively calling ourselves...\n",fname); 
        free(fname); 
        // call ourselves again... 
        return check_dir(dirname,cfgname, found_path,onetime); 
      }
    }
  }

  //ok, no once directory, just look directly for the file
  free(fname); 
  asprintf(&fname,"%s/%s", dirname, cfgname); 

  FILE *maybe = check_file(fname); 

  if (maybe) 
  {
    if (found_path) *found_path = fname; 
    else free(fname); 
    if (onetime) *onetime = 0;
    return maybe; 
  }

  free(fname); 
  return NULL; 
}


FILE* find_config(const char * cfgname, const char * cfgpath, char ** found_path, int * onetime) 
{
  //first check if cfgpath is a file 
  if (cfgpath != NULL ) 
  {
    //is it a file? 
    FILE * maybe= check_file(cfgpath); 
    if (maybe) 
    {
      if (found_path) *found_path = strdup(cfgpath); 
      if (onetime) *onetime = 0; 
      return maybe; 
    }
    else 
    {
        FILE * maybe = check_dir(cfgpath, cfgname, found_path,onetime); 
        if (maybe) return maybe; 
    }
    fprintf(stderr,"Could not find %s or anything like %s in %s/; ignoring passed path and moving to defaults\n", cfgpath, cfgname,cfgpath); 
  }

  //check CWD 
  FILE * maybe = check_dir(".",cfgname,found_path,onetime); 
  if (maybe) return maybe; 

  //check ${RNO_G_INSTALL_DIR}/cfg 
  char * envdir = getenv("RNO_G_INSTALL_DIR"); 
  if (envdir !=NULL) 
  {
    char * dirname = 0;
    asprintf(&dirname,"%s/cfg", envdir);
    maybe = check_dir(dirname,cfgname, found_path,onetime); 
    free(dirname); 
    if (maybe) return maybe; 
  }

  maybe = check_dir("/rno-g/cfg", cfgname, found_path, onetime); 
  if (maybe) return maybe; 

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
