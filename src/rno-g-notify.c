// program to grab notify events, and send notifications

#define _GNU_SOURCE
#include "ice-common.h"
#include "ice-notify.h" 
#include <stdio.h> 
#include <stdlib.h> 
#include <signal.h> 
#include <sys/stat.h>
#include <sys/inotify.h> 
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h> 
#include <dirent.h> 
#include <curl/curl.h> 

#define SLACK_WEBHOOK_FILE "/rno-g/var/notify/slack-webhook"

#define DATA_SIZE 256 
#define MAXMSG_SIZE 140 

CURL * curl  = NULL; 
char *webhook; 

volatile int die = 0; 
int station_number = -1; 
int outbox_maybe_dirty = 1; 

int lock_fd; 
int inbox_fd; 
int outbox_fd;  
int sent_fd; 
DIR * foutbox; 

char data[DATA_SIZE+1]; 
char msg_data[MAXMSG_SIZE+1]; 

static int nodot(const struct dirent * d)
{
  return d->d_name[0] != '.';
}

void sigdie(int signum) 
{
  (void) signum; 
  die = 1;
}

static int datesort(const struct dirent ** a, const struct dirent ** b)
{
  static struct stat stat_a; 
  static struct stat stat_b; 

  fstatat(outbox_fd, (*a)->d_name,&stat_a, 0);
  fstatat(outbox_fd, (*b)->d_name,&stat_b, 1);

  return stat_a.st_ctim.tv_sec + stat_a.st_ctim.tv_nsec*1e-9 < stat_b.st_ctim.tv_sec + stat_b.st_ctim.tv_nsec * 1e-9; 
}


int process_file(const char * file) 
{
  if (die) return 1; 
  int fd = openat(outbox_fd, file, O_RDONLY); 
  if (fd <=0) return 1; 
  ssize_t sz = read(fd, msg_data, MAXMSG_SIZE);
  if (sz > MAXMSG_SIZE) 
  {
    fprintf(stderr,"Warning: %s truncated\n", file); 
  }

  size_t data_size = snprintf(data,sizeof(data),"{\"text\": *FROM STATION %d* [%s]: %s\n}", station_number, file, msg_data);
  if(data_size > DATA_SIZE) data_size = DATA_SIZE; 

  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data_size); 

  CURLcode code = curl_easy_perform(curl); 

  close(fd); 
  if (code) 
  {
    fprintf(stderr, "curl returned: %d (%s) on message %s.\n", code, curl_easy_strerror(code), data);
  }
  return code; 
}

void empty_outbox() 
{
  if (!outbox_maybe_dirty) return; 

  struct dirent ** dlist = 0; 
  int nentries = scandirat(outbox_fd,".", &dlist, nodot, datesort); 
  int fail = 0; 
  for (int i = 0; i < nentries; i++) 
  {
    if (die) break; 
    struct dirent * d = dlist[i]; 
    if (!fail && !process_file(d->d_name))
    {
        renameat(outbox_fd, d->d_name, sent_fd, d->d_name); 
        fsync(sent_fd); // ??? expensive ??? 
    }
    else
    {
        fail++; 
    }

    free(d->d_name); 
  }

  if (!fail) outbox_maybe_dirty = 0; 
}

void send_to_outbox(const char * file) 
{

  //first check if it's there. There is a slight potential race condition if 
  //a file is moved in after 
  if (access(file,R_OK)) return; 

  // move it to outbox, try to empty outbox
  renameat(inbox_fd, file, outbox_fd, file); 
  fsync(outbox_fd); //is this necessary? 
  outbox_maybe_dirty = 1;
  empty_outbox(); 
}


int main() 
{
  //check lockfile

  lock_fd = open(RNO_G_ICE_NOTIFY_LOCKFILE, O_CREAT | O_RDWR); 
  if (flock(lock_fd, LOCK_EX | LOCK_NB))
  {
    fprintf(stderr,"Could not acquire lock!!!\n"); 
    return 1; 

  }

  //load webhook 

  FILE * fwebhook = fopen(SLACK_WEBHOOK_FILE,"r"); 
  if (!fwebhook) 
  {
    fprintf(stderr, SLACK_WEBHOOK_FILE " is not defined. Exiting\n"); 
    return 1; 
  }

  fscanf(fwebhook,"%ms", &webhook); 
  fclose(fwebhook); 

  //load station id 
  station_number = get_station_number(); 

  if (station_number < 0) 
  {
    fprintf(stderr,"Hmm, we have negative station number? Perhaps we couldn't read it in?\n"); 
  }
  // open inbox/outbox/sent fd 

  inbox_fd = open(RNO_G_ICE_NOTIFY_INBOX, O_RDONLY); 
  outbox_fd = open(RNO_G_ICE_NOTIFY_OUTBOX, O_RDONLY); 
  sent_fd = open(RNO_G_ICE_NOTIFY_OUTBOX, O_RDONLY); 

  if (!inbox_fd || !outbox_fd || !sent_fd) 
  {
    fprintf(stderr,"Could not open notify directories!!!\n"); 

  }

  foutbox = fdopendir(outbox_fd); 

  //set up curl
  curl = curl_easy_init(); 
  curl_easy_setopt(curl, CURLOPT_URL, webhook); 
  curl_easy_setopt(curl, CURLOPT_POST,1); 
  struct curl_slist *hlist = NULL; 
  curl_slist_append(hlist, "Content-type: application/json"); 
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,hlist); 
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data); 


  int inotify_fd = inotify_init1(IN_NONBLOCK); 

  //set up inotify watch
  int watch_fd  = inotify_add_watch(inotify_fd, RNO_G_ICE_NOTIFY_INBOX, IN_MOVED_TO); 

  if (watch_fd <= 0) 
  {
    fprintf(stderr,"Could not set up inotify watch..."); 
  }

  // attempt to empty outbox
  empty_outbox(); 

  // loop over all existing files first 
  DIR * D = fdopendir(inbox_fd); 

  struct dirent * d = 0; 
  while(1) 
  {
    d = readdir(D); 
    if (!d) break; 

    if (d->d_name[0] != '.') 
      send_to_outbox(d->d_name); 
  }

  empty_outbox(); 

  //now we just read the watches
  
  static char watch_buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event)))); 
  const struct inotify_event * ev; 

  signal(SIGINT, sigdie);
  signal(SIGQUIT, sigdie);

  while (!die) 
  {
    int len = read(watch_fd, watch_buf, sizeof(watch_buf)); 
    if (len > 0) 
    {
      char * ptr; 

      for (ptr = watch_buf; ptr < watch_buf+len; ptr += sizeof(struct inotify_event) + ev->len)
      {
        ev = (const struct inotify_event*) ptr; 
        if (ev->mask & IN_MOVED_TO && ev->len)
        {
          send_to_outbox(ev->name);
        }
      }
      empty_outbox(); 
    }
    else
    {
      if (die) break; 
      sleep(10); 
      if (die) break; 
      empty_outbox(); //try to empty the outbox again... 
    }
  }

  close(inotify_fd); 
  close(inbox_fd); 
  close(outbox_fd); 
  close(sent_fd); 

  return 0;


}


