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
    char * ifname = args[2]; 
    FILE * f = fopen(ifname,"r"); 

    acq_config_t cfg;
    read_acq_config(f,&cfg); 
    char *ofname = nargs > 3 ? args[3] : "acq-checked.cfg"; 
    FILE * of = fopen(ofname,"w"); 
    dump_acq_config(of,&cfg); 
    fclose(f); 
    fclose(of); 
    return 0; 
  }

  fprintf(stderr,"I don't know how to make a %s config", args[1] ); 
  return 1; 
}
