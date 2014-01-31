#ifndef _HTTP_CLIENT_H_
#define _HTTP_CLIENT_H_

#include <sys/types.h>
#include <sys/time.h>
#include <stdio.h>
#include <event.h>
#include <stdbool.h>

#include <trema.h>


enum {
  HTTP_METHOD_GET = 0,
  HTTP_METHOD_POST,
  HTTP_METHOD_PUT,
  HTTP_METHOD_DELETE,

  HTTP_METHOD_NUM,
};

enum {
  HTTP_TRANSACTION_SUCCEEDED = 0,
  HTTP_TRANSACTION_FAILED = -1,
};

typedef struct {
  char content_type[128];
  buffer *body;
} http_content;


typedef void (*http_resp_handler)(int status, int code,
                                  const http_content *content, void *cb_arg);

extern bool do_http_request( int method,
                             const char *uri,
                             const http_content *content,
                             http_resp_handler cb,
                             void *cb_arg );
extern bool init_http_client(const void *);
extern bool finalize_http_client(void);

/* new function */
extern bool init_http_client_new(void (*exit_cb)(void *), void *arg, time_t response_to, time_t connect_to);
extern bool finalize_http_client_new(void);
extern bool stop_http_client_new(void);


/* New tools */
extern http_content *create_http_content(const char *content_type,
                                         const void *body, size_t length);
extern void free_http_content(http_content *content);
extern bool set_type_http_content(http_content *content, const char *type);
extern bool append_body_http_content(http_content *content,
                                     const void *body, size_t len);


#endif	/* !_HTTP_CLIENT_H_ */
