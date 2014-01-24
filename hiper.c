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

#include <trema.h>

#include "hiper.h"

static void clean_completed_http_transaction(http_th_info_t *th_info);


#define EASY_SETOPT(_e, _o, _p) {                       \
    CURLcode _rc = curl_easy_setopt((_e), (_o), (_p));  \
    if (_rc != CURLE_OK) {                              \
      TRACE("ERROR: curl_easy_setopt in %s returns %s", \
            __func__, curl_easy_strerror(_rc));         \
      goto error;                                       \
    }                                                   \
  }

#define MULTI_SETOPT(_m, _o, _p) {                              \
    CURLMcode _rc = curl_multi_setopt((_m), (_o), (_p));        \
    if (_rc != CURLM_OK) {                                      \
      TRACE("ERROR: curl_multi_setopt in %s returns %s",        \
            __func__, curl_multi_strerror(_rc));                \
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
    TRACE("ERROR: curl_multi_socket_action in %s returns %s",
            func, curl_multi_strerror(rc));
  }
  return ret;
}
#define	MULTI_ACTION(_h, _s, _e, _r)	multi_action((_h),(_s),(_e),(_r), __func__)

static inline int
multi_add_easy(CURLM *multi,
               CURL *easy,
               const char *func)
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

/*****************************************************************************
 * Multi Timer
 *****************************************************************************/
/* Called by libevent when our timeout expires */
static void
multi_timer_cb(void *arg)
{
  http_th_info_t *th_info = arg;

  TRACE("Polling Timer Expired----->");

  th_info->timer = false;

  if (MULTI_ACTION(th_info->multi, CURL_SOCKET_TIMEOUT, 0, &th_info->running))
    exit(1);

  clean_completed_http_transaction(th_info);

  TRACE("<-----Polling Timer Expired: running:%d", th_info->running);
}


/* Update the event timer after curl_multi library calls */
static int
multi_timer_update(CURLM *multi __attribute__((unused)),
                   long timeout_ms,
                   http_th_info_t *th_info)
{
  TRACE("%p %ld ms", th_info, timeout_ms);
  if (timeout_ms >= 0) {
    struct itimerspec to;

    memset(&to, 0, sizeof(to));
    to.it_value.tv_sec = timeout_ms / 1000;
    to.it_value.tv_nsec = (timeout_ms % 1000) * 1000L * 1000L;

    if (th_info->timer) {
      delete_timer_event(multi_timer_cb, th_info);
      th_info->timer = false;
    }
    if (!add_timer_event_callback(&to, multi_timer_cb, th_info))
      TRACE("ERROR: add_timer_event_callback");
    else
      th_info->timer = true;
  } else if (th_info->timer) {
    delete_timer_event(multi_timer_cb, th_info);
    th_info->timer = false;
  }
  return 0;
}

/*****************************************************************************
 * Http_Transaction
 *****************************************************************************/
void
destroy_http_transaction(http_transaction_t *trans)
{
  TRACE("trans:%p", trans);

  if (trans) {
    if (trans->sock >= 0) {
      delete_fd_handler(trans->sock);
      trans->sock = -1;
    }

    if (trans->easy) {
      curl_multi_remove_handle(trans->th_info->multi, trans->easy);
      curl_easy_cleanup(trans->easy);
      trans->easy = NULL;
    }

    if (trans->slist) {
      curl_slist_free_all( trans->slist );
      trans->slist = NULL;
    }

    if (trans->cb) {
      (trans->cb)(trans->status, trans->code, trans->response, trans->arg);
      trans->cb = NULL;
      trans->arg = NULL;
    }

    if (trans->request) {
      free_http_content(trans->request);
      trans->request = NULL;
    }
    if (trans->response) {
      free_http_content(trans->response);
      trans->response = NULL;
    }
    if (trans->url) {
      FREE(trans->url);
      trans->url = NULL;
    }
    FREE(trans);
  }
}

/* Check for completed transfers, and remove their easy handles */
static void
clean_completed_http_transaction(http_th_info_t *th_info)
{
  CURLMsg *msg;
  int msgs_left;

  TRACE("REMAINING: %d", th_info->running);
  while ((msg = curl_multi_info_read(th_info->multi, &msgs_left))) {

    if (msg->msg == CURLMSG_DONE) {
      http_transaction_t *trans;
      CURL *easy;
      CURLcode rc;
      int code = 0;
      char *type = NULL;

      rc = msg->data.result;

      easy = msg->easy_handle;
      curl_easy_getinfo(easy, CURLINFO_PRIVATE, &trans);
      //      curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff_url);

      if (curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK) {
        TRACE("code:%d", code);
        trans->code = code;
      }
      if (code >= 200 && code < 300)
        trans->status = HTTP_TRANSACTION_SUCCEEDED;

      if (curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &type) ==  CURLE_OK) {
        if (type)
          trans->response->type = STRDUP(type);
      }

      TRACE("DONE: %s => (%d) %s", trans->url, rc, trans->error);

      destroy_http_transaction(trans);
    } else {
      TRACE("XXX: msg:%d", msg->msg);
    }
  }
}

static size_t
trans_write_cb(void *ptr __attribute__((unused)),
               size_t size,
               size_t nmemb,
               void *data)
{
  size_t realsize = size * nmemb;
  http_transaction_t *trans = data;

  TRACE("trans:%p", trans);
  return realsize;
}

http_transaction_t *
create_http_transaction(http_th_info_t *th_info,
                        const char *url)
{
  http_transaction_t *trans;

  if ((trans = MALLOC(sizeof(*trans))) != NULL) {
    memset(trans, 0, sizeof(*trans));
    trans->error[0] = '\0';
    trans->th_info = th_info;
    strncpy(trans->url, url, sizeof(trans->url) - 1);
    trans->url[sizeof(trans->url) - 1] = '\0';
    trans->sock = -1;

    if ((trans->easy = curl_easy_init()) == NULL)
      goto error;

    //    EASY_SETOPT(trans->easy, CURLOPT_DNS_CACHE_TIMEOUT, 0L);
    EASY_SETOPT(trans->easy, CURLOPT_TCP_NODELAY, 1L);
    EASY_SETOPT(trans->easy, CURLOPT_TIMEOUT, 10L);
    EASY_SETOPT(trans->easy, CURLOPT_CONNECTTIMEOUT, 3L);

    EASY_SETOPT(trans->easy, CURLOPT_URL, trans->url);
    EASY_SETOPT(trans->easy, CURLOPT_WRITEFUNCTION, trans_write_cb);
    EASY_SETOPT(trans->easy, CURLOPT_WRITEDATA, trans);
    //    EASY_SETOPT(trans->easy, CURLOPT_VERBOSE, 1L);
    EASY_SETOPT(trans->easy, CURLOPT_ERRORBUFFER, trans->error);
    EASY_SETOPT(trans->easy, CURLOPT_PRIVATE, trans);
    EASY_SETOPT(trans->easy, CURLOPT_NOPROGRESS, 1L);

    if (MULTI_ADD_EASY(th_info->multi, trans->easy))
      goto error;

    if (0) {
  error:
      destroy_http_transaction(trans);
      trans = NULL;
    }
  }
  TRACE("trans:%p %s", trans, url);
  return trans;
}




static void
trans_handler_raw(int sock,
                  int action,
                  http_transaction_t *trans)
{
  http_th_info_t *th_info = trans->th_info;

  TRACE("trans:%p sock:%d action:0x%x", trans, sock, action);

  if (MULTI_ACTION(th_info->multi, sock, action, &th_info->running))
    destroy_http_transaction(trans);

  clean_completed_http_transaction(th_info);

  if (th_info->running <= 0) {
    if (th_info->timer) {
      TRACE("last transfer done, kill timeout");
      delete_timer_event(multi_timer_cb, th_info);
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

static inline int
update_http_transaction(http_transaction_t *trans,
                        curl_socket_t sock,
                        int action)
{
  CURLMcode rc;
  http_th_info_t *th_info = trans->th_info;
  const char *whatstr[] = { "none", "IN", "OUT", "INOUT", "REMOVE" };

  if (trans->sock < 0) {
    rc = curl_multi_assign(th_info->multi, sock, trans);
    if (rc != CURLM_OK) {
      TRACE("ERROR: curl_multi_assign in %s returns %s",
            __func__, curl_multi_strerror(rc));
      return -1;
    }
    trans->sock = sock;
    set_fd_handler(sock, trans_handler_in, trans, trans_handler_out, trans);
  }
  set_readable(sock, action & CURL_POLL_IN);
  set_writable(sock, action & CURL_POLL_OUT);

  TRACE("trans:%p sock:%d Changing action from %s to %s",
        trans, sock, whatstr[trans->action], whatstr[action]);

  trans->action = action;
  return 0;
}


static int
update_http_transaction_handler(CURL *easy,
                           curl_socket_t sock,
                           int action,
                           void *multi_arg,
                           void *easy_arg)
{
  http_th_info_t *th_info = multi_arg;
  http_transaction_t *trans = easy_arg;
  int ret = 0;

  TRACE("easy:%p sock:%d trans:%p action:%d---->", easy, sock, trans, action);
  assert(th_info);

  if (action == CURL_POLL_REMOVE) {
    if (trans->sock >= 0) {
      delete_fd_handler(trans->sock);
      trans->sock = -1;
    }
    trans->action = action;
  } else {
    if (!trans)
      curl_easy_getinfo(easy, CURLINFO_PRIVATE, &trans);
    ret = update_http_transaction(trans, sock, action);
  }
  TRACE("<-------------ret:%d", ret);
  return ret;
}


/***************************************************************************/
void
destroy_http_th(http_th_info_t *th_info)
{
  if (th_info) {
    if (th_info->timer) {
      delete_timer_event(multi_timer_cb, th_info);
      th_info->timer = false;
    }
    if (th_info->multi)
      curl_multi_cleanup(th_info->multi);
    FREE(th_info);
  }
}

http_th_info_t *
create_http_th(void)
{
  http_th_info_t *th_info;

  if ((th_info = MALLOC(sizeof(*th_info))) != NULL) {
    memset(th_info, 0, sizeof(*th_info));

    if ((th_info->multi = curl_multi_init()) == NULL)
      goto error;

    /* setup the generic multi interface options we want */
    MULTI_SETOPT(th_info->multi, CURLMOPT_SOCKETFUNCTION, update_http_transaction_handler);
    MULTI_SETOPT(th_info->multi, CURLMOPT_SOCKETDATA, th_info);
    MULTI_SETOPT(th_info->multi, CURLMOPT_TIMERFUNCTION, multi_timer_update);
    MULTI_SETOPT(th_info->multi, CURLMOPT_TIMERDATA, th_info);

    if (0) {
    error:
      destroy_http_th(th_info);
      th_info = NULL;
    }
  }
  return th_info;
}


/****************************************************************************
 * VNP like
 ***************************************************************************/
static http_th_info_t *HTTP_THREAD_INFO;

bool
init_http_client(void)
{
  if (HTTP_THREAD_INFO)
    return false;
  if ((HTTP_THREAD_INFO = create_http_th()) != NULL)
    return true;
  return false;
}

bool
finalize_http_client(void)
{
  if (!HTTP_THREAD_INFO)
    return false;
  destroy_http_th(HTTP_THREAD_INFO);
  HTTP_THREAD_INFO = NULL;
  return true;
}


static size_t
read_from_buffer( void *ptr,
                  size_t size,
                  size_t nmemb,
                  void *arg )
{
  http_content_t *content = arg;
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
write_to_buffer( void *contents,
                 size_t size,
                 size_t nmemb,
                 void *arg )
{
  http_content_t *content = arg;
  buffer *buf = NULL;

  TRACE( "Writing contents ( contents:%p size:%zu nmemb:%zu arg:%p ).",
         contents, size, nmemb, arg );

  size_t real_size = size * nmemb;

  if (!content->body)
    content->body = alloc_buffer_with_length( 1024 );

  if ((buf = content->body) != NULL) {
    void *p = append_back_buffer( buf, real_size );
    if (p && real_size)
      memcpy( p, contents, real_size );
  }

  TRACE( "%zu bytes data is written into buffer ( buf:%p length:%zu ).",
         real_size, buf, buf->length );

  return real_size;
}


#define USER_AGENT "Bisco/0.0.3"

bool
do_http_request( int method,
                 const char *uri,
                 const http_content_t *content,
                 http_resp_handler cb,
                 void *cb_arg )
{
  http_transaction_t *trans;

  if ((trans = MALLOC(sizeof(*trans))) != NULL) {
    struct curl_slist *slist = NULL;
    long content_length = 0;

    memset(trans, 0, sizeof(*trans));
    trans->error[0] = '\0';
    trans->th_info = HTTP_THREAD_INFO;
    trans->sock = -1;
    if ((trans->url = STRDUP(uri)) == NULL)
      goto error;

    if (content) {
      trans->request = alloc_http_content(content->type, content->body);
    } else {
      trans->request = alloc_http_content(NULL, NULL);
    }
    if (!trans->request)
      goto error;

    trans->response = alloc_http_content(NULL, NULL);
    if (!trans->response)
      goto error;

    if ((trans->easy = curl_easy_init()) == NULL)
      goto error;

    if (trans->request->body && trans->request->body->length)
      content_length = (long) trans->request->body->length - 1L;

    EASY_SETOPT(trans->easy, CURLOPT_TCP_NODELAY, 1L);
    EASY_SETOPT(trans->easy, CURLOPT_TIMEOUT, 10L);
    EASY_SETOPT(trans->easy, CURLOPT_CONNECTTIMEOUT, 3L);

    EASY_SETOPT(trans->easy, CURLOPT_USERAGENT, USER_AGENT );
    EASY_SETOPT(trans->easy, CURLOPT_URL, trans->url);
    EASY_SETOPT(trans->easy, CURLOPT_NOPROGRESS, 1L);

    EASY_SETOPT(trans->easy, CURLOPT_WRITEFUNCTION, write_to_buffer);
    EASY_SETOPT(trans->easy, CURLOPT_WRITEDATA, trans->response);

    EASY_SETOPT(trans->easy, CURLOPT_ERRORBUFFER, trans->error);
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
      TRACE("ERROR: Undefined HTTP request method ( %d ).", method );
      goto error;
    }

    if ( trans->request->body && trans->request->body->length ) {
      char buff[ 128 ];
      const char *p;

      if ( trans->request->type && strlen( trans->request->type ) )
        p = trans->request->type;
      else
        p = "text/plain";

      snprintf(buff, sizeof(buff), "Content-Type: %s", p);
      slist = curl_slist_append( trans->slist, buff );
      if (!slist)
        goto error;
      trans->slist = slist;

      EASY_SETOPT(trans->easy, CURLOPT_READFUNCTION, read_from_buffer);
      EASY_SETOPT(trans->easy, CURLOPT_READDATA, trans->request );
    }

    if (trans->slist)
      EASY_SETOPT(trans->easy, CURLOPT_HTTPHEADER, trans->slist);

    if (MULTI_ADD_EASY(trans->th_info->multi, trans->easy))
      goto error;

    /* all ok */
    trans->cb = cb;
    trans->arg = cb_arg;

    if (0) {
  error:
      destroy_http_transaction(trans);
      trans = NULL;
    }
  }
  TRACE("trans:%p %s", trans, uri);
  if (trans)
    return true;
  return false;
}


