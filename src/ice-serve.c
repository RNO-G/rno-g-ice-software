#include "ice-serve.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <errno.h>


struct ice_serve_ctx
{
  ice_serve_setup_t setup;
  int serve_fd;
  int nheaders;
  ice_serve_http_header_t * headers;
  char * reqbuf;
  //CAREFUL ABOUT ALIGNMENT HERE!!!!
  char data[];
};

int echo_handler(const ice_serve_request_t * req, ice_serve_response_t * resp, void *udata)
{
  (void) udata;
  resp->code = ICE_SERVE_OK;
  resp->content_type = "text/plain";
  resp->content = req->resource;
  return 0;
}

static ice_serve_setup_t default_setup = { .port = 1056, .handler = echo_handler};

ice_serve_ctx_t * ice_serve_init(const ice_serve_setup_t *setup)
{
  int serve_fd;
  struct sockaddr_in saddr;

  serve_fd = socket(AF_INET,SOCK_STREAM,0);
  if (serve_fd < 0)
  {
    fprintf(stderr,"Socket error\n");
    return NULL;
  }
  int opt = 1;
  setsockopt(serve_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, (socklen_t)sizeof(int));

  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = INADDR_ANY;
  if (!setup) setup = &default_setup;
  saddr.sin_port = htons(setup->port);

  int bind_ok = bind(serve_fd, (struct sockaddr*)  &saddr, sizeof(saddr));
  if (bind_ok < 0)
  {
    close(serve_fd);
    fprintf(stderr,"Couldn't bind to %hu\n", setup->port);
    return NULL;
  }

  int listen_ok = listen(serve_fd,10);
  if (listen_ok < 0)
  {
    fprintf(stderr,"Couldn't listen");
    close(serve_fd);
    return NULL;
  }

  int reqsize = setup->reqbuf_size ?: 512;
  int max_headers = setup->max_headers ?: 16;

  ice_serve_ctx_t * ctx = malloc(sizeof(ice_serve_ctx_t) + max_headers * sizeof(ice_serve_http_header_t) + reqsize);
  if (!ctx)
  {
    fprintf(stderr,"Couldn't allocate memory\n");
    close(serve_fd);
    return NULL;
  }

  ctx->serve_fd = serve_fd;
  memcpy(&ctx->setup, setup, sizeof(*setup));
  ctx->setup.reqbuf_size = reqsize;
  ctx->setup.max_headers = max_headers;
  // TODO: check to make sure alignment is ok; I think it should be
  ctx->headers = (ice_serve_http_header_t*) ctx->data;
  ctx->reqbuf = ctx->data + max_headers * sizeof(ice_serve_http_header_t);
  return ctx;
}



static const char * msg400 = "HTTP/1.1 400 Bad Request\r\nConnection: close";
static const char * msg404 = "HTTP/1.1 404 Not Found\r\nConnection: close";
static const char * msg500 = "HTTP/1.1 500 Internal Server Error\r\nConnection: close";
static const char * msg501 = "HTTP/1.1 501 Not Implemented\r\nConnection: close";


int ice_serve_run(ice_serve_ctx_t * ctx)
{
  int nrequests = 0;
  while ( !ctx->setup.exit_sentinel || !(*ctx->setup.exit_sentinel))
  {
    struct pollfd pfd = {.fd = ctx->serve_fd, .events = POLLIN};

    if (!poll(&pfd, 1,1000)) //timeout
      continue;

    if (pfd.revents != POLLIN) continue;

    struct sockaddr_in caddr;
    socklen_t caddrlen = sizeof(caddr);
    int  client_fd = accept(ctx->serve_fd,(struct sockaddr*) &caddr, &caddrlen);
    if (client_fd <0) continue;

    ssize_t rcvd = recv(client_fd, ctx->reqbuf, ctx->setup.reqbuf_size-1,0);
    if (rcvd > 0)
    {
      ctx->reqbuf[rcvd]=0;

      //we are a very dumb HTTP server
      char * GET = strstr(ctx->reqbuf,"GET ");
      if (GET != ctx->reqbuf)
      {
           write(client_fd, msg400, strlen(msg400));
           continue;
      }
      char * HTTP = strstr(ctx->reqbuf," HTTP/1");
      char * CRNL = strstr(ctx->reqbuf,"\r\n");
      if (!HTTP || CRNL < HTTP)
      {
           write(client_fd, msg400, strlen(msg400));
           continue;
      }

      char * resource = GET + 4;

      HTTP[0] = 0;//null terminate URI and headers

      // parse headers
      int iheader = 0;
      char * uagent = NULL;
      char * host = NULL;
      while (CRNL)
      {
        *CRNL = 0;
        char * maybe_key = CRNL+2;
        char * COLON = strstr(maybe_key,":");
        char * NEXT_CRNL = strstr(CRNL+2,"\r\n");
        if (COLON)
        {


          //find first non-whitespace char for value
          char * val = COLON+1;
          while (*val==' ') val++;
          *COLON=0;

          if (iheader < ctx->setup.max_headers)
          {
            ctx->headers[iheader].key = maybe_key;
            ctx->headers[iheader].val = val;
            iheader++;
          }

          //get these even if we've exceeded max header
          if (!strcmp(maybe_key,"Host")) host = val;
          if (!strcmp(maybe_key,"User-Agent")) uagent = val;
        }
        CRNL = NEXT_CRNL;
      }


      ice_serve_request_t req = {.resource = resource, .uagent = uagent, .host = host, .nheaders = iheader, .headers = ctx->headers};

      ice_serve_response_t resp = {.code = ICE_SERVE_OK, .content_type = "text/html"};

      if (ctx->setup.handler(&req,&resp, ctx->setup.userdata))
      {
        resp.code = ICE_SERVE_ERROR;
      }


      if (resp.code != ICE_SERVE_OK)
      {
        switch(resp.code)
        {
          case ICE_SERVE_BAD_REQUEST:
            write(client_fd, msg400,strlen(msg400)); break;
          case ICE_SERVE_NOT_FOUND:
            write(client_fd, msg404,strlen(msg404)); break;
          case ICE_SERVE_UNIMPLEMENTED:
            write(client_fd, msg501,strlen(msg501)); break;
          default:
            write(client_fd, msg500,strlen(msg500)); break;
        }
      }
      else
      {

        if (!resp.content_type) resp.content_type="text/plain";

        char hdrbuf[512];
        int content_len = resp.content_length ?: strlen(resp.content);
        int len = snprintf(hdrbuf,sizeof(hdrbuf),"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n", resp.content_type, content_len+2);
        if (len == sizeof(hdrbuf-1))
        {
          fprintf(stderr,"header too long, bad content-type?\n");
        }
        else
        {
          write(client_fd,hdrbuf,len);
          if (resp.content)
          {
            write(client_fd,resp.content, content_len);
            write(client_fd,"\n\n",2);
          }
        }
      }

      close(client_fd);

      nrequests++;
      if (resp.free_fun)  resp.free_fun(resp.free_fun_arg);
    }
  }

  return nrequests;
}

void ice_serve_destroy(ice_serve_ctx_t * ctx)
{

  close(ctx->serve_fd);
  free(ctx);
}

