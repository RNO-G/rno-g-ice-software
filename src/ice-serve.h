#ifndef _ICE_SERVE_H
#define _ICE_SERVE_H

#include <stdint.h>

/**
 *  Minimalist http/1.1 server for in-memory data
 *
 */
typedef struct ice_serve_http_header
{
  const char * key;
  const char * val;
} ice_serve_http_header_t;



typedef struct ice_serve_request
{
  const char * resource;
  const char * uagent;
  const char * host;
  uint16_t nheaders;
  const ice_serve_http_header_t * headers;
} ice_serve_request_t;


typedef struct ice_serve_response
{
  enum
  {
    ICE_SERVE_OK,
    ICE_SERVE_BAD_REQUEST,
    ICE_SERVE_NOT_FOUND,
    ICE_SERVE_ERROR,
    ICE_SERVE_UNIMPLEMENTED
  } code;
  const char * content_type;
  const char * content;
  uint16_t content_length; //0 to use strlen

  // it may be necessary to free the data after sending it, if dynamically allocated. this will get called if non-NULL
  // after content is written.
  void (*free_fun)(void *);
  void * free_fun_arg;
} ice_serve_response_t;

typedef int (*ice_serve_handler_t) (const ice_serve_request_t * req, ice_serve_response_t *response, void * userdata);

typedef struct ice_serve_setup
{
  uint16_t port;
  uint16_t reqbuf_size; //size of request buf for reading headers, 0 for default (most headers are ignored anyway...).
  uint16_t max_headers; // maximum number of parsed headers
  volatile int * exit_sentinel; // if not null, setting the value pointed to this by non-zero will stop run
  ice_serve_handler_t handler;
  void * userdata;
} ice_serve_setup_t;


typedef struct ice_serve_ctx ice_serve_ctx_t;


/* Initializes a context, making  acopy of setup
 *
 **/
ice_serve_ctx_t * ice_serve_init(const ice_serve_setup_t *setup);
int ice_serve_run(ice_serve_ctx_t * ctx);
void ice_serve_destroy(ice_serve_ctx_t * ctx);

#endif


