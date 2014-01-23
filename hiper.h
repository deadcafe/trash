#ifndef _HIPER_H_
#define	_HIPER_H_

#include <stdio.h>
#include <event.h>
#include <curl/curl.h>

# if 1
#  include <stdlib.h>
#  include <syslog.h>
#  define _log(pri_,fmt_,...)     fprintf(stdout,fmt_,##__VA_ARGS__)
#  define LOG(pri_,fmt_,...)      _log((pri_),"%s:%d:%s " fmt_ "\n", __FILE__,__LINE__,__func__, ##__VA_ARGS__)
#  define TRACE(fmt_,...)         LOG(LOG_DEBUG,fmt_,##__VA_ARGS__)

# endif

#define	MALLOC(_s)	malloc((_s))
#define	FREE(_p)	free((_p))


/* HTTP client thread info */
typedef struct {
  struct event_base *evbase;
  struct event *timer_ev;
  CURLM *multi;
  int running;
} http_th_info_t;


typedef struct {
  CURL *easy;
  struct event *ev;
  http_th_info_t *th_info;

  curl_socket_t sock;
  int action;

  char url[1024];
  char error[CURL_ERROR_SIZE];
} http_transaction_t;

extern void destroy_http_th(http_th_info_t *th_info);
extern http_th_info_t *create_http_th(struct event_base *base);

extern http_transaction_t *create_http_transaction(http_th_info_t *th_info,
                                                   const char *url);
extern void destroy_http_transaction(http_transaction_t *trans);

#endif	/* !_HIPER_H_ */
