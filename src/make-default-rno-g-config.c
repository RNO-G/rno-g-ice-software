#include "ice-config.h" 
#include <stdio.h> 
#include <string.h> 


int main(int nargs, char ** args) 
{
  if (nargs < 1) 
  {
    fprintf(stderr,"What type of config?\n"); 
    return 1; 
  }

  if (!strcmp(args[1],"acq"))
  {
    char *of = nargs > 2 ? args[2] : "acq.cfg"; 
    FILE * f = fopen(of,"w"); 
    acq_config_t cfg; 
    init_acq_config(&cfg); 
    dump_acq_config(f,&cfg); 
    fclose(f); 
    return 0; 
  }

  fprintf(stderr,"I don't know how to make a %s config", args[1] ); 
  return 1; 
}
