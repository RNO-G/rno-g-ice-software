#include "ice-config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int main(int nargs, char ** args)
{
  if (nargs < 5)
  {
    fprintf(stderr,"What type of config?\n");
    fprintf(stderr," Usage: overlay-rno-g-config  type  fragment0  [fragment1 .. ]  output\n");
    fprintf(stderr," Example : overlay-rno-g-config  acq /rno-g/cfg/acq.cfg  /rno-g/cfg/fragments/cal0.cfg  /rno-g/cfg/acq.once/acq-cal0.cfg\n");
    return 1;
  }

  if (!strcmp(args[1],"acq"))
  {
    int nconfigs = nargs -3 ;
    FILE ** configs =  calloc(nconfigs, sizeof(FILE*));
    for (int iconfig = 0; iconfig < nconfigs; iconfig++)
    {
      configs[iconfig] = fopen(args[2+iconfig],"w");
    }

    acq_config_t cfg;
    init_acq_config(&cfg);
    read_acq_configs(nconfigs, configs ,&cfg);

    char *ofname =  args[nargs-1];
    FILE * of = fopen(ofname,"w");
    if (!of) of = stdout;
    dump_acq_config(of,&cfg);

    for (int iconfig = 0; iconfig <nconfigs; iconfig++) fclose(configs[iconfig]);
    if (of!= stdout) fclose(of);
    free(configs);
    return 0;
  }

  fprintf(stderr,"I don't know how to make a %s config\n", args[1] );
  return 1;
}
