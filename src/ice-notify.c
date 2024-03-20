#include "ice-notify.h" 
#define _GNU_SOURCE
#include <stdio.h> 
#include <sys/file.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h> 
#include <time.h> 


static char buf[sizeof(RNO_G_ICE_NOTIFY_INBOX) + 512]; 

static int inbox_fd = 0;

static char hostname[65]; 


void rno_g_notify(const char * msg) 
{
  struct timespec ts; 
  clock_gettime(CLOCK_REALTIME, &ts); 
  pid_t pid = getpid(); 

  if (!hostname[0]) 
  {
    gethostname(hostname,sizeof(hostname)-1); 

  }
  if (!inbox_fd) 
  {
    inbox_fd = open(RNO_G_ICE_NOTIFY_INBOX,O_RDONLY); 
  }


  unsigned sec = ts.tv_sec;
  unsigned nsec = ts.tv_nsec;
  snprintf(buf, sizeof(buf), ".%s-%s-p%u-%u.%u", hostname, program_invocation_short_name, pid, sec, nsec); 

  int fd = openat(inbox_fd, buf, O_CREAT | O_WRONLY, S_IRUSR | S_IRGRP | S_IROTH); 
  write(fd, msg, strlen(msg)); 
  fsync(fd); 
  close(fd); 
  renameat(inbox_fd, buf, inbox_fd, buf+1); 
  fsync(inbox_fd); 
}





