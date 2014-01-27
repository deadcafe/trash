#ifndef _HTTP_CLIENT_H_
#define _HTTP_CLIENT_H_

#include <sys/queue.h>
#include <stdio.h>
#include <event.h>
#include <stdbool.h>
#include <pthread.h>
#include <curl/curl.h>

#include <trema.h>
#include "func_queue.h"

# if 0
#  include <stdlib.h>
#  include <syslog.h>
#  define _log(pri_,fmt_,...)     fprintf(stdout,fmt_,##__VA_ARGS__)
#  define LOG(pri_,fmt_,...)      _log((pri_),"%s:%d:%s() " fmt_ "\n", __FILE__,__LINE__,__func__, ##__VA_ARGS__)
#  define TRACE(fmt_,...)         LOG(LOG_DEBUG,fmt_,##__VA_ARGS__)

# endif

enum {
  HTTP_METHOD_INVALID = 0,

  HTTP_METHOD_GET,
  HTTP_METHOD_POST,
  HTTP_METHOD_PUT,
  HTTP_METHOD_DELETE,

  HTTP_METHOD_NUM,
};

enum {
  HTTP_TRANSACTION_FAILED = 0,
  HTTP_TRANSACTION_SUCCEEDED = 1,
};

typedef struct {
  char *type;
  buffer *body;
} http_content_t;


/* HTTP client thread info */
typedef struct {
  long response_timeout;		/* msec */
  long connect_timeout;			/* msec */

  CURLM *multi;
  int running;
  bool timer;

  func_q_t *func_q;
  pthread_barrier_t barrier;
} http_th_info_t;


typedef void (*http_resp_handler)(int status, int code,
                                  const http_content_t *content, void *cb_arg);


typedef struct _http_transaction_t {
  CURL *easy;
  http_th_info_t *th_info;

  curl_socket_t sock;
  int action;

  int method;
  http_content_t *request;
  struct {
    http_content_t *content;
    int status;
    int code;

    http_resp_handler cb;
    void *arg;
  } response;

  struct curl_slist *slist;

  char *url;
  char error[CURL_ERROR_SIZE];
} http_transaction_t;





extern bool do_http_request( int method,
                             const char *uri,
                             const http_content_t *content,
                             http_resp_handler cb,
                             void *cb_arg );
extern bool init_http_client(long response_to, long connect_to);
extern bool finalize_http_client(void);
extern bool stop_http_client(void);

#endif	/* !_HTTP_CLIENT_H_ */
