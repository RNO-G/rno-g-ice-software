#include "ice-serve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>


static int handler(const ice_serve_request_t *req, ice_serve_response_t *resp, void * udata)
{
  (void) udata;
  printf("Resource: %s\nHost: %s\nUA: %s\n", req->resource, req->host ?: "(none)", req->uagent ?: "(none)");

  int len = 64;
  if (strlen(req->resource)>1) len= atoi(req->resource +1);

  resp->code = ICE_SERVE_OK;
  char * buf = malloc(len+1);
  resp->content = buf;
  resp->content_length = len;
  memset(buf,'x', len);
  resp->free_fun = free;
  resp->free_fun_arg = buf;
  return 0;

}
static volatile int quit = 0;

static void sighandler(int signum)
{
  quit =1;
  printf("got signal %d\n", signum);
}


int main(int nargs, char ** args)
{

  int port = 10000;
  if (nargs > 1) port = atoi(args[1]);
  ice_serve_setup_t  setup = {.port=port, .handler = handler, .exit_sentinel = &quit};
  ice_serve_ctx_t * ctx = ice_serve_init(&setup);
  if (!ctx)
  {
    return 1;
  }
  signal(SIGINT, sighandler);
  printf("%d requests\n", ice_serve_run(ctx));
  ice_serve_destroy(ctx);
  return 0;
}
