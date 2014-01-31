#include <sys/time.h>
#include <sys/stat.h>
#include <sys/poll.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <curl/curl.h>

#include "func_queue.h"
#include "http_client.h"

//#define ENABLE_TRACE
#include "private_log.h"


#define	HTTP_TIMEOUT	60L
#define CONNECT_TIMEOUT	(HTTP_TIMEOUT / 2L)



/* HTTP client thread info */
typedef struct {
  long response_timeout;		/* msec */
  long connect_timeout;			/* msec */

  CURLM *multi;
  int running;
  bool timer;

  void (*exit_cb)(void *);
  void *arg;

  func_q_t *func_q;
  pthread_barrier_t barrier;
} http_th_info_t;

typedef struct {
  http_content *content;
  int status;
  int code;

  http_resp_handler cb;
  void *arg;
  char error[CURL_ERROR_SIZE];
} http_response_t;


typedef struct _http_transaction_t {
  CURL *easy;
  http_th_info_t *th_info;

  curl_socket_t sock;
  int action;

  int method;
  http_content *request;
  http_response_t *res;

  struct curl_slist *slist;
} http_transaction_t;


#define	FREE(_p)	xfree((_p))
#define	MALLOC(_s)	xmalloc((_s))

static void clean_completed_transaction(http_th_info_t *th_info);
static void reply_from_client(void *arg);

static http_th_info_t *HTTP_THREAD_INFO;
static pthread_t http_client_thread;


bool is_enable_libevent_wrapper(void) __attribute__((weak));

bool
is_enable_libevent_wrapper(void)
{
  return false;
}

/********************************************************************
 * common functions
 ********************************************************************/
static inline char *
strdup_raw(const char *src) {
  size_t len = strlen(src) + 1;
  char *dst = MALLOC(len);
  if (dst)
    memcpy(dst, src, len);
  return dst;
}

#define EASY_SETOPT(_e, _o, _p) {                       \
    CURLcode _rc = curl_easy_setopt((_e), (_o), (_p));  \
    if (_rc != CURLE_OK) {                              \
      WARN("ERROR: curl_easy_setopt returns %s",        \
           curl_easy_strerror(_rc));                    \
      goto error;                                       \
    }                                                   \
  }


static void
destroy_http_response(http_response_t *res)
{
  TRACE("res:%p", res);
  if (res) {
    res->cb = NULL;
    res->arg = NULL;
    if (res->content) {
      TRACE("content:%p", res->content);
      free_http_content(res->content);
      res->content = NULL;
    }
    FREE(res);
  }
}


static http_response_t *
create_http_response(http_resp_handler cb,
                     void *arg)
{
  http_response_t *res = MALLOC(sizeof(*res));
  if (res) {
    res->cb = cb;
    res->arg = arg;
    res->status = -1;
    res->code = HTTP_TRANSACTION_FAILED;
    if ((res->content = create_http_content(NULL, NULL, 0)) == NULL) {
      destroy_http_response(res);
      res = NULL;
    } else {
      TRACE("content:%p", res->content);
    }
  }
  TRACE("res:%p", res);
  return res;
}

static void
destroy_transaction(http_transaction_t *trans)
{
  TRACE("trans:%p", trans);

  if (trans) {
    if (trans->sock >= 0) {
      set_readable_safe(trans->sock, false);
      set_writable_safe(trans->sock, false);
      delete_fd_handler(trans->sock);
      trans->sock = -1;
    }

    if (trans->easy) {
      if (trans->th_info) {
        curl_multi_remove_handle(trans->th_info->multi, trans->easy);
        trans->th_info = NULL;
      }
      curl_easy_cleanup(trans->easy);
      trans->easy = NULL;
    }

    if (trans->slist) {
      curl_slist_free_all( trans->slist );
      trans->slist = NULL;
    }

    if (trans->request) {
      free_http_content(trans->request);
      trans->request = NULL;
    }

    if (trans->res) {
      destroy_http_response(trans->res);
      trans->res = NULL;
    }
    FREE(trans);
  }
}





/********************************************************************
 * http client thread functions
 ********************************************************************/
#define MULTI_SETOPT(_m, _o, _p) {                              \
    CURLMcode _rc = curl_multi_setopt((_m), (_o), (_p));        \
    if (_rc != CURLM_OK) {                                      \
      WARN("ERROR: curl_multi_setopt returns %s",               \
           curl_multi_strerror(_rc));                           \
      goto error;                                               \
    }                                                           \
  }


static inline int
multi_action(CURLM *multi,
             curl_socket_t sock,
             int action,
             int *running,
             const char *func)
{
  int ret = -1;
  CURLMcode rc = curl_multi_socket_action(multi, sock, action, running);

  switch (rc) {
  case CURLM_OK:
    ret = 0;
    break;

  case CURLM_BAD_SOCKET:
    ret = 0;

  default:
    ERROR("ERROR: curl_multi_socket_action in %s returns %s",
          func, curl_multi_strerror(rc));
  }
  return ret;
}
#define	MULTI_ACTION(_h, _s, _e, _r)	multi_action((_h),(_s),(_e),(_r), __func__)

static inline int
multi_add_easy(CURLM *multi,
               CURL *easy,
               const char *func __attribute__((unused)))
{
  int ret = -1;
  CURLMcode rc = curl_multi_add_handle(multi, easy);

  switch (rc) {
  case CURLM_OK:
    ret = 0;
    break;

  case CURLM_BAD_SOCKET:
    ret = 0;

  default:
    TRACE("ERROR: curl_multi_add_handle in %s returns %s",
          func, curl_multi_strerror(rc));
  }
  return ret;
}
#define	MULTI_ADD_EASY(_m, _e)	multi_add_easy((_m), (_e), __func__)


static void
multi_timer_cb(void *arg)
{
  http_th_info_t *th_info = arg;

  TRACE("Polling Timer Expired %p:%p----->", multi_timer_cb, th_info);
  th_info->timer = false;

  if (MULTI_ACTION(th_info->multi, CURL_SOCKET_TIMEOUT, 0, &th_info->running))
    exit(1);

  clean_completed_transaction(th_info);
}


static int
multi_timer_update(CURLM *multi __attribute__((unused)),
                   long timeout_ms,
                   http_th_info_t *th_info)
{
  TRACE("%p %ld ms", th_info, timeout_ms);

  if (timeout_ms < 0)
    return 0;

  if (th_info->timer) {
    delete_timer_event_safe(multi_timer_cb, th_info);
    th_info->timer = false;
  }

  struct itimerspec to;

  memset(&to, 0, sizeof(to));
  to.it_value.tv_sec = timeout_ms / 1000;
  to.it_value.tv_nsec = (timeout_ms % 1000) * 1000L * 1000L;
  to.it_value.tv_nsec += 1;

  if (add_timer_event_callback_safe(&to, multi_timer_cb, th_info))
    th_info->timer = true;
  else {
    TRACE("ERROR: add_timer_event_callback");
  }
  return 0;
}


static void
done_transaction(http_th_info_t *th_info,
                 CURLcode rc,
                 CURL *easy,
                 http_response_t *res)
{
  char *url;

  curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &url);
  if (rc == CURLE_OK) {
    int code;

    if (curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK) {
      char *type;
      res->code = code;

      if (code >= 200 && code < 300)
        res->status = HTTP_TRANSACTION_SUCCEEDED;

      if (curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &type) ==  CURLE_OK)
        set_type_http_content(res->content, type);
    }
  }

  if (res->status == HTTP_TRANSACTION_SUCCEEDED) {
    DEBUG("DONE: %s => code:%d status:%d", url, res->code, res->status);
  } else {
    NOTICE("failed %s => (%d) %s code:%d status:%d",
           url, rc, res->error, res->code, res->status);
  }

  if (!func_q_request(th_info->func_q, DIR_TO_DOWN, reply_from_client, res))
    destroy_http_response(res);

  curl_easy_cleanup(easy);
}

static void
clean_completed_transaction(http_th_info_t *th_info)
{
  CURLMsg *msg;
  int *msgs_left = &th_info->running;

  while ((msg = curl_multi_info_read(th_info->multi, msgs_left))) {
    INFO("REMAINING: %d", th_info->running);

    if (msg->msg == CURLMSG_DONE) {
      http_transaction_t *trans;
      CURL *easy;
      CURLcode rc;
      http_response_t *res;

      rc = msg->data.result;
      easy = msg->easy_handle;

      curl_easy_getinfo(easy, CURLINFO_PRIVATE, &trans);
      assert(trans);

      curl_multi_remove_handle(trans->th_info->multi, easy);
      trans->easy = NULL;
      trans->th_info = NULL;

      res = trans->res;
      trans->res = NULL;

      done_transaction(th_info, rc, easy, res);
      destroy_transaction(trans);
    }
  }
}


static void
trans_handler_raw(int sock,
                  int action,
                  http_transaction_t *trans)
{
  http_th_info_t *th_info = trans->th_info;

  TRACE("trans:%p sock:%d action:0x%x", trans, sock, action);

  if (MULTI_ACTION(th_info->multi, sock, action, &th_info->running))
    destroy_transaction(trans);

  clean_completed_transaction(th_info);

  if (th_info->running <= 0) {
    if (th_info->timer) {
      delete_timer_event_safe(multi_timer_cb, th_info);
      th_info->timer = false;
    }
  }
}


static void
trans_handler_in(int sock,
                 void *arg)
{
  http_transaction_t *trans = arg;
  trans_handler_raw(sock, CURL_CSELECT_IN, trans);
}


static void
trans_handler_out(int sock,
                  void *arg)
{
  http_transaction_t *trans = arg;
  trans_handler_raw(sock, CURL_CSELECT_OUT, trans);
}


static int
update_transaction(CURL *easy,
                   curl_socket_t sock,
                   int action,
                   void *multi_arg,
                   void *easy_arg)
{
  http_th_info_t *th_info = multi_arg;
  http_transaction_t *trans = easy_arg;
  const char *whatstr[] = { "none", "IN", "OUT", "INOUT", "REMOVE" };
  int ret = 0;

  TRACE("easy:%p sock:%d trans:%p action:%d---->", easy, sock, trans, action);
  assert(th_info);

  if (!trans)
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &trans);

  if (action == CURL_POLL_REMOVE) {
    if (trans->sock >= 0) {
      set_readable_safe(trans->sock, false);
      set_writable_safe(trans->sock, false);
      delete_fd_handler_safe(trans->sock);
      trans->sock = -1;
    }
  } else {
    if (trans->sock < 0) {
      CURLMcode rc;

      rc = curl_multi_assign(th_info->multi, sock, trans);
      if (rc != CURLM_OK) {
        ERROR("curl_multi_assign() returns %s", curl_multi_strerror(rc));
        return -1;
      }
      trans->sock = sock;
      set_fd_handler_safe(sock, trans_handler_in, trans, trans_handler_out, trans);
    }
    set_readable_safe(sock, action & CURL_POLL_IN);
    set_writable_safe(sock, action & CURL_POLL_OUT);

  }
  DEBUG("trans:%p sock:%d Changing action from %s to %s",
        trans, sock, whatstr[trans->action], whatstr[action]);
  trans->action = action;
  return ret;
}


static size_t
read_from_buffer( void *ptr,
                  size_t size,
                  size_t nmemb,
                  void *arg )
{
  http_content *content = arg;
  buffer *buf = content->body;
  size_t real_size = size * nmemb;
  size_t len = 0;

  TRACE("Reading contents ( ptr:%p size:%zu nmemb:%zu arg: %p ).",
        ptr, size, nmemb, arg );

  if ( buf )
    len = buf->length;

  if ( len < real_size )
    real_size = len;

  if (real_size) {
    memcpy( ptr, buf->data, real_size );
    remove_front_buffer( buf, real_size );
  }

  TRACE( "%zu bytes data is read from buffer ( buf = %p, length = %zu ).",
         real_size, buf, buf->length );
  return real_size;
}


static size_t
write_to_buffer( void *src,
                 size_t size,
                 size_t nmemb,
                 void *arg )
{
  http_content *content = arg;

  TRACE( "Writing contents src:%p size:%zu nmemb:%zu content:%p",
         src, size, nmemb, content );
  size_t real_size = size * nmemb;

  assert(content);
  append_body_http_content(content, src, real_size);
  return real_size;
}


static void *
http_client_th_entry( void *arg )
{
  http_th_info_t *th_info = arg;

  TRACE("start http client thread");
  if (!is_enable_libevent_wrapper())
    add_thread();

  init_event_handler_safe();
  init_signal_handler_safe();
  init_timer_safe();

  func_q_bind(th_info->func_q, DIR_TO_UP, EH_TYPE_SAFE);

  pthread_barrier_wait(&th_info->barrier);

  DEBUG("going event handler loop");
  start_event_handler_safe();
  DEBUG("break out event handler loop");

  func_q_unbind(th_info->func_q, DIR_TO_UP);

  if (th_info->exit_cb)
    func_q_request(th_info->func_q, DIR_TO_DOWN,
                   th_info->exit_cb, th_info->arg);

  finalize_timer_safe();
  finalize_signal_handler_safe();
  finalize_event_handler_safe();

  INFO("END http client thread");
  return arg;
}


static void
request_from_main(void *arg)
{
  http_transaction_t *trans = arg;

  if (trans) {
    TRACE("trans:%p", trans);

    if (MULTI_ADD_EASY(trans->th_info->multi, trans->easy)) {
      if (!func_q_request(trans->th_info->func_q, DIR_TO_DOWN,
                          reply_from_client, trans->res)) {
        TRACE("failed to exec func_q_request()");
        trans->th_info = NULL;
        destroy_transaction(trans);
      }
    }
  }
}

/*
 * func_q handler
 */
static void
stop_from_main(void *arg)
{
  http_th_info_t *th_info = arg;
  assert(th_info);

  TRACE("stoping thread");
  stop_event_handler_safe();

  /* XXX: leak remaining transaction, sorry (;-p */
  /* maybe use transaction list */
}


/*****************************************************************************
 * main thread function
 *****************************************************************************/
static void
destroy_http_th(http_th_info_t *th_info)
{
  if (th_info) {
    TRACE("th_info:%p", th_info);

    if (th_info->timer) {
      delete_timer_event_safe(multi_timer_cb, th_info);
      th_info->timer = false;
    }

    if (th_info->func_q) {
      TRACE("func_q:%p", th_info->func_q);
      func_q_destroy(th_info->func_q);
      th_info->func_q = NULL;
    }

    if (th_info->multi)
      curl_multi_cleanup(th_info->multi);

    pthread_barrier_destroy(&th_info->barrier);
    FREE(th_info);
  }
}


static http_th_info_t *
create_http_th(void (*exit_cb)(void *),
               void *arg,
               time_t response_to,
               time_t connect_to)
{
  http_th_info_t *th_info;

  if ((th_info = MALLOC(sizeof(*th_info))) != NULL) {
    memset(th_info, 0, sizeof(*th_info));

    if (pthread_barrier_init(&th_info->barrier, NULL, 2)) {
      WARN("failed pthread_barrier_init()");
      goto error;
    }

    if ((th_info->func_q = func_q_create()) == NULL)
      goto error;

    if ((th_info->multi = curl_multi_init()) == NULL) {
      WARN("failed curl_multi_init()");
      goto error;
    }

    /* setup the generic multi interface options we want */
    MULTI_SETOPT(th_info->multi, CURLMOPT_SOCKETFUNCTION, update_transaction);
    MULTI_SETOPT(th_info->multi, CURLMOPT_SOCKETDATA, th_info);
    MULTI_SETOPT(th_info->multi, CURLMOPT_TIMERFUNCTION, multi_timer_update);
    MULTI_SETOPT(th_info->multi, CURLMOPT_TIMERDATA, th_info);

    th_info->exit_cb = exit_cb;
    th_info->arg = arg;

    if (!func_q_bind(th_info->func_q, DIR_TO_DOWN, EH_TYPE_GENERIC))
      goto error;

    th_info->response_timeout = (long) response_to;
    th_info->connect_timeout = (long) connect_to;

    if (0) {
    error:
      destroy_http_th(th_info);
      th_info = NULL;
    }
  }
  return th_info;
}


/*
 * func_q handler
 */
static void
reply_from_client(void *arg) {
  http_response_t *res = arg;

  TRACE("res:%p", res);
  if (res->cb)
    res->cb(res->status, res->code, res->content, res->arg);
  destroy_http_response(res);
}


/*****************************************************************************
 * API
 *****************************************************************************/
bool
init_http_client(const void *unused __attribute__((unused)))
{
  return init_http_client_new(NULL, NULL, HTTP_TIMEOUT, CONNECT_TIMEOUT);
}


bool
finalize_http_client(void)
{
  return false;
}


bool
init_http_client_new(void (*exit_cb)(void *),
                     void *arg,
                     time_t response_to,
                     time_t connect_to)
{
  DEBUG("cb:%p arg:%p response:%u connect:u",
        exit_cb, arg, response_to, connect_to);

  if (HTTP_THREAD_INFO || !response_to || !connect_to )
    return false;

  if ((HTTP_THREAD_INFO = create_http_th(exit_cb, arg,
                                         response_to, connect_to)) != NULL) {
    pthread_attr_t attr;
    int ret;

    pthread_attr_init( &attr );
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
    ret = pthread_create( &http_client_thread, &attr,
                          http_client_th_entry, HTTP_THREAD_INFO );
    if (ret) {
      finalize_http_client();
      return false;
    }
    pthread_barrier_wait(&HTTP_THREAD_INFO->barrier);
    return true;
  }
  return false;
}


bool
finalize_http_client_new(void)
{
  DEBUG("");
  if (HTTP_THREAD_INFO) {
    http_th_info_t *th_info = HTTP_THREAD_INFO;

    HTTP_THREAD_INFO = NULL;

    func_q_unbind(th_info->func_q, DIR_TO_DOWN);
    destroy_http_th(th_info);

    return true;
  } else {
    NOTICE("nothing http client thread");
  }
  return false;
}


bool
stop_http_client_new(void)
{
  DEBUG("");
  if (HTTP_THREAD_INFO) {
    http_th_info_t *th_info = HTTP_THREAD_INFO;

    func_q_request(th_info->func_q, DIR_TO_UP, stop_from_main, th_info);
    return true;
  } else {
    NOTICE("nothing http client thread");
  }
  return false;
}


#define USER_AGENT "Bisco/0.0.3"

bool
do_http_request( int method,
                 const char *url,
                 const http_content *request,
                 http_resp_handler cb,
                 void *cb_arg )
{
  http_transaction_t *trans;
  DEBUG("method:%d url:%s content:%p cb:%p arg:%p",
        method, url, request, cb, cb_arg);

  if ((trans = MALLOC(sizeof(*trans))) != NULL) {
    struct curl_slist *slist = NULL;
    long content_length = 0;

    memset(trans, 0, sizeof(*trans));
    trans->th_info = HTTP_THREAD_INFO;
    trans->sock = -1;

    trans->res = create_http_response(cb, cb_arg);
    if (!trans->res)
      goto error;

    if ((trans->easy = curl_easy_init()) == NULL) {
      WARN("failed curl_easy_init()");
      goto error;
    }

    if (request) {
      trans->request = create_http_content(request->content_type,
                                           request->body->data,
                                           request->body->length);
      if (!trans->request)
        goto error;

      char buff[ 128 ];
      const char *p;

      if ( trans->request->content_type )
        p = trans->request->content_type;
      else
        p = "text/plain";

      snprintf(buff, sizeof(buff), "Content-Type: %s", p);
      slist = curl_slist_append( trans->slist, buff );
      if (!slist) {
        WARN("failed curl_slist_append()");
        goto error;
      }
      trans->slist = slist;

      EASY_SETOPT(trans->easy, CURLOPT_READFUNCTION, read_from_buffer);
      EASY_SETOPT(trans->easy, CURLOPT_READDATA, trans->request );

      if (trans->request->body && trans->request->body->length)
        content_length = (long) trans->request->body->length - 1L;
    }

    TRACE("");

    EASY_SETOPT(trans->easy, CURLOPT_TCP_NODELAY, 1L);
    EASY_SETOPT(trans->easy, CURLOPT_TIMEOUT, trans->th_info->response_timeout);
    EASY_SETOPT(trans->easy, CURLOPT_CONNECTTIMEOUT,
                trans->th_info->connect_timeout);

    EASY_SETOPT(trans->easy, CURLOPT_USERAGENT, USER_AGENT );
    EASY_SETOPT(trans->easy, CURLOPT_URL, url);
    EASY_SETOPT(trans->easy, CURLOPT_NOPROGRESS, 1L);

    EASY_SETOPT(trans->easy, CURLOPT_WRITEFUNCTION, write_to_buffer);
    EASY_SETOPT(trans->easy, CURLOPT_WRITEDATA, trans->res->content);

    EASY_SETOPT(trans->easy, CURLOPT_ERRORBUFFER, trans->res->error);
    EASY_SETOPT(trans->easy, CURLOPT_PRIVATE, trans);

    switch (method) {
    case HTTP_METHOD_GET:
      EASY_SETOPT(trans->easy, CURLOPT_HTTPGET, 1L );
      break;

    case HTTP_METHOD_PUT:
      EASY_SETOPT(trans->easy, CURLOPT_UPLOAD, 1L );
      EASY_SETOPT(trans->easy, CURLOPT_INFILESIZE, content_length );
      slist = curl_slist_append( trans->slist, "Expect:" );
      if (!trans->slist)
        goto error;
      trans->slist = slist;
      break;

    case HTTP_METHOD_POST:
      EASY_SETOPT(trans->easy, CURLOPT_POST, 1L );
      EASY_SETOPT(trans->easy, CURLOPT_POSTFIELDSIZE, content_length);
      break;

    case HTTP_METHOD_DELETE:
      EASY_SETOPT(trans->easy, CURLOPT_CUSTOMREQUEST, "DELETE" );
      break;

    default:
      WARN("INVALID: Undefined HTTP request method ( %d ).", method );
      goto error;
    }

    if (trans->slist)
      EASY_SETOPT(trans->easy, CURLOPT_HTTPHEADER, trans->slist);

    if (!func_q_request(trans->th_info->func_q, DIR_TO_UP,
                        request_from_main, trans)) {
      ERROR("failed to exec func_q_request()");
      goto error;
    }

    if (0) {
  error:
      destroy_transaction(trans);
      trans = NULL;
    }
  }
  TRACE("trans:%p %s", trans, url);
  if (trans)
    return true;
  return false;
}


void
free_http_content(http_content *content)
{
  DEBUG("content:%p", content);
  if (content) {
    if (content->body) {
      free_buffer(content->body);
      content->body = NULL;
    }
    FREE(content);
  }
}


http_content *
create_http_content(const char *type,
                    const void *body,
                    size_t length)
{
  http_content *content = MALLOC(sizeof(*content));
  if (content) {
    memset(content, 0, sizeof(*content));

    if ((content->body = alloc_buffer()) == NULL)
      goto error;

    if (!set_type_http_content(content, type))
      goto error;

    if (!append_body_http_content(content, body, length))
      goto error;

    if (0) {
    error:
      free_http_content(content);
      content = NULL;
    }
  }
  DEBUG("content:%p type:%p body:%p len:%ul", content, type, body, length);
  return content;
}


bool
set_type_http_content(http_content *content,
                      const char *type)
{
  DEBUG("content:%p type:%s", content, type);

  if (type)
    snprintf(content->content_type, sizeof(content->content_type), "%s" , type);
  return true;
}


bool
append_body_http_content(http_content *content,
                         const void *body, size_t len)
{
  DEBUG("content:%p body:%p len:%zu", content, body, len);
  if (body && len) {
    char *p = append_back_buffer(content->body, len);
    if (!p) {
      ERROR("failed append_back_buffer:%p len:%zu", content->body, len);
      return false;
    }
    memcpy(p, body, len);
  }
  return true;
}
