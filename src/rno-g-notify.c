#include "ice-notify.h" 
#include <stdio.h> 
#include <string.h> 

static char msg[RNO_G_MAXMSG_SIZE+1]; 

int main(int nargs, char **args) 
{
  //if we have arguments, concatenate them together into a message... 


  int len = 0; 

  if (nargs > 1 ) 
  {


    for (int arg = 1; arg < nargs; arg++) 
    {
      int this_len = strlen(args[arg]); 
      int cpy_len = this_len + len < RNO_G_MAXMSG_SIZE ? this_len : RNO_G_MAXMSG_SIZE - (this_len + len); 
      memcpy(msg + len, args[arg], cpy_len); 
      len += this_len; 
      if (len > 140) 
      {
        fprintf(stderr,"WARNING: message is longer than 140 characters. Will be truncated\n"); 
        break; 
      }
    }
  }
  else
  {
    while (!feof(stdin))
    {
      len += fread(msg, 1, RNO_G_MAXMSG_SIZE - len, stdin); 
    }
  }

  if (!len) 
  {
    fprintf(stderr, "No message detected... Usage: rno-g-notify MSG or read from stdin\n"); 
    return 1; 
  }

  rno_g_notify(msg); 
  return 0; 
}
