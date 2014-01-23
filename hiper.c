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


#include "hiper.h"

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


static inline short
action2event(int action)
{
  short events = 0;

  if (action & CURL_POLL_IN)
    events |= EV_READ;
  if (action & CURL_POLL_OUT)
    events |= EV_WRITE;

  if (events)
    events |= EV_PERSIST;
  return events;
}

static inline int
event2action(short event)
{
  int action = 0;

  if (event & EV_READ)
    action |= CURL_CSELECT_IN;
  if (event & EV_WRITE)
    action |= CURL_CSELECT_OUT;

  return action;
}

/*****************************************************************************
 * Http_Transaction
 *****************************************************************************/
void
destroy_http_transaction(http_transaction_t *trans)
{
  TRACE("trans:%p", trans);

  if (trans) {
    if (trans->ev) {
      if (event_pending(trans->ev, 0, NULL)) {
        event_del(trans->ev);
        TRACE("trans:%p del ev", trans);
      }
      event_free(trans->ev);
      trans->ev = NULL;
      TRACE("trans:%p FREE ev", trans);
    }

    if (trans->easy) {
      curl_multi_remove_handle(trans->th_info->multi, trans->easy);
      curl_easy_cleanup(trans->easy);
      trans->easy = NULL;
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
      char *eff_url;
      http_transaction_t *trans;
      CURL *easy;
      CURLcode rc;

      rc = msg->data.result;

      easy = msg->easy_handle;
      curl_easy_getinfo(easy, CURLINFO_PRIVATE, &trans);
      curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff_url);

      TRACE("DONE: %s => (%d) %s", eff_url, rc, trans->error);

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

    trans->ev = event_new(th_info->evbase, -1, 0, NULL, trans);
    if (!trans->ev)
      goto error;

    if ((trans->easy = curl_easy_init()) == NULL)
      goto error;

    EASY_SETOPT(trans->easy, CURLOPT_URL, trans->url);
    EASY_SETOPT(trans->easy, CURLOPT_WRITEFUNCTION, trans_write_cb);
    EASY_SETOPT(trans->easy, CURLOPT_WRITEDATA, trans);
    //    EASY_SETOPT(trans->easy, CURLOPT_VERBOSE, 1L);
    EASY_SETOPT(trans->easy, CURLOPT_ERRORBUFFER, trans->error);
    EASY_SETOPT(trans->easy, CURLOPT_PRIVATE, trans);
    EASY_SETOPT(trans->easy, CURLOPT_NOPROGRESS, 0L);

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
trans_handler(int sock,
              short events,
              void *arg)
{
  http_transaction_t *trans = arg;
  http_th_info_t *th_info = trans->th_info;
  int action;

  TRACE("trans:%p sock:%d events:0x%x", trans, sock, events);

  action = event2action(events);
  if (MULTI_ACTION(th_info->multi, sock, action, &th_info->running))
    destroy_http_transaction(trans);

  clean_completed_http_transaction(th_info);

  if (th_info->running <= 0) {
    if (evtimer_pending(th_info->timer_ev, NULL)) {
      TRACE("last transfer done, kill timeout");
      evtimer_del(th_info->timer_ev);
    }
  }
}

static inline int
update_http_transaction(http_transaction_t *trans,
                   curl_socket_t sock,
                   int action)
{
  short events;
  CURLMcode rc;
  http_th_info_t *th_info = trans->th_info;
  const char *whatstr[] = { "none", "IN", "OUT", "INOUT", "REMOVE" };

  events = action2event(action);
  if (trans->ev) {
    short old = action2event(trans->action);
    if (event_pending(trans->ev, old, NULL)) {
      event_del(trans->ev);
      TRACE("trans:%p del ev", trans);
    }
    event_assign(trans->ev, th_info->evbase, sock, events, trans_handler, trans);
    event_add(trans->ev, NULL);
  }

  if (trans->sock < 0) {
    rc = curl_multi_assign(th_info->multi, sock, trans);
    if (rc != CURLM_OK) {
      TRACE("ERROR: curl_multi_assign in %s returns %s",
            __func__, curl_multi_strerror(rc));
      event_del(trans->ev);
      return -1;
    }
    trans->sock = sock;
  }

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
    if (trans->ev) {
      short old = action2event(trans->action);
      if (event_pending(trans->ev, old, NULL)) {
        event_del(trans->ev);
        TRACE("trans:%p del ev", trans);
      }
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

/*****************************************************************************
 * Multi Timer
 *****************************************************************************/
/* Called by libevent when our timeout expires */
static void
multi_timer_cb(int fd __attribute__((unused)),
               short events,
               void *arg)
{
  http_th_info_t *th_info = arg;

  TRACE("Polling Timer Expired----->: 0x%x", events);

  if (MULTI_ACTION(th_info->multi, CURL_SOCKET_TIMEOUT, 0, &th_info->running))
    exit(1);

  clean_completed_http_transaction(th_info);

  TRACE("<-----Polling Timer Expired: 0x%x running:%d",
        events, th_info->running);
}


/* Update the event timer after curl_multi library calls */
static int
multi_timer_update(CURLM *multi __attribute__((unused)),
                   long timeout_ms,
                   http_th_info_t *th_info)
{
  TRACE("%p %ld ms", th_info, timeout_ms);
  if (timeout_ms >= 0) {
    struct timeval timeout;

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    evtimer_add(th_info->timer_ev, &timeout);
  }
  return 0;
}

/***************************************************************************/
void
destroy_http_th(http_th_info_t *th_info)
{
  if (th_info) {
    if (th_info->timer_ev)
      event_free(th_info->timer_ev);
    if (th_info->multi)
      curl_multi_cleanup(th_info->multi);
    FREE(th_info);
  }
}

http_th_info_t *
create_http_th(struct event_base *base)
{
  http_th_info_t *th_info;

  if ((th_info = MALLOC(sizeof(*th_info))) != NULL) {
    memset(th_info, 0, sizeof(*th_info));
    th_info->evbase = base;

    if ((th_info->multi = curl_multi_init()) == NULL)
      goto error;

    if ((th_info->timer_ev = evtimer_new(th_info->evbase, multi_timer_cb, th_info)) == NULL)
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
