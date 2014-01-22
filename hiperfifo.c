#include <sys/time.h>
#include <sys/stat.h>
#include <sys/poll.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <event2/event.h>
#include <curl/curl.h>



# if 1
#  include <stdio.h>
#  include <stdlib.h>
#  include <syslog.h>
#  define       _log(pri_,fmt_,...)     fprintf(stdout,fmt_,##__VA_ARGS__)
#  define       LOG(pri_,fmt_,...)      _log((pri_),"%s:%d:%s " fmt_ "\n", __FILE__,__LINE__,__func__, ##__VA_ARGS__)
#  define       TRACE(fmt_,...)         LOG(LOG_DEBUG,fmt_,##__VA_ARGS__)

# endif



/* Global information, common to all connections */
typedef struct _GlobalInfo_t {
  struct event_base *evbase;
  struct event *fifo_ev;
  struct event *timer_ev;
  CURLM *multi;
  int running;
  FILE* input;
} GlobalInfo_t;


/* Information associated with a specific easy handle */
typedef struct _ConnInfo_t {
  CURL *easy;
  char *url;
  GlobalInfo_t *global;
  char error[CURL_ERROR_SIZE];
} ConnInfo_t;


/* Information associated with a specific socket */
typedef struct _SockInfo_t {
  curl_socket_t sock;
  CURL *easy;
  int action;
  long timeout;
  struct event *ev;
  GlobalInfo_t *global;
} SockInfo_t;


/*****************************************************************************
 * Multi event
 *****************************************************************************/
/* Check for completed transfers, and remove their easy handles */
static void
clean_completed_handle(GlobalInfo_t *ginfo)
{
  CURLMsg *msg;
  int msgs_left;

  TRACE("REMAINING: %d", ginfo->running);
  while ((msg = curl_multi_info_read(ginfo->multi, &msgs_left))) {

    if (msg->msg == CURLMSG_DONE) {
      char *eff_url;
      ConnInfo_t *cinfo;
      CURL *easy;
      CURLcode rc;

      rc = msg->data.result;

      easy = msg->easy_handle;
      curl_easy_getinfo(easy, CURLINFO_PRIVATE, &cinfo);
      curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff_url);

      TRACE("DONE: %s => (%d) %s", eff_url, rc, cinfo->error);

      curl_multi_remove_handle(ginfo->multi, easy);
      free(cinfo->url);
      curl_easy_cleanup(easy);
      free(cinfo);
    }
  }
  
}

static inline int
multi_action(CURLM *multi,
             curl_socket_t sock,
             int ev_bitmask,
             int *running,
             const char *func)
{
  int ret = -1;
  CURLMcode rc = curl_multi_socket_action(multi, sock, ev_bitmask, running);
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

/* Called by libevent when we get action on a multi socket */
static void
event_cb(int fd,
         short events,
         void *arg)
{
  GlobalInfo_t *ginfo = arg;
  int action = 0;

  TRACE("%p 0x%x", ginfo, events);

  if (events & EV_READ)
    action |= CURL_CSELECT_IN;
  if (events & EV_WRITE)
    action |= CURL_CSELECT_OUT;

  if (MULTI_ACTION(ginfo->multi, fd, action, &ginfo->running))
    exit(1);

  clean_completed_handle(ginfo);

  if (ginfo->running <= 0) {
    if (evtimer_pending(ginfo->timer_ev, NULL)) {
      TRACE("last transfer done, kill timeout");
      evtimer_del(ginfo->timer_ev);
    }
  }
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
  GlobalInfo_t *ginfo = arg;

  TRACE("Polling Timer Expired-->: 0x%x", events);

  clean_completed_handle(ginfo);

  if (MULTI_ACTION(ginfo->multi, CURL_SOCKET_TIMEOUT, 0, &ginfo->running))
    exit(1);

  TRACE("<--Polling Timer Expired: 0x%x running:%d",
        events, ginfo->running);
}


/* Update the event timer after curl_multi library calls */
static int
multi_timer_update(CURLM *multi __attribute__((unused)),
                   long timeout_ms,
                   GlobalInfo_t *ginfo)
{
  TRACE("%p %ld ms", ginfo, timeout_ms);
  if (timeout_ms >= 0) {
    struct timeval timeout;

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    evtimer_add(ginfo->timer_ev, &timeout);
    return 0;
  }
  return -1;
}


/*****************************************************************************
 * Sock
 *****************************************************************************/
/* Clean up the SockInfo structure */
static int
rm_sock(SockInfo_t *sinfo)
{
  TRACE("%p", sinfo);
  if (sinfo) {
    if (sinfo->ev) { //sinfo->evset -> sinfo->ev
      event_free(sinfo->ev);
      sinfo->ev = NULL;
    }
    free(sinfo);
  }
  return 0;
}


/* Assign information to a SockInfo structure */
static int
set_sock(SockInfo_t *sinfo,
         curl_socket_t sock,
         CURL *easy,
         int act,
         GlobalInfo_t *ginfo)
{
  short events = EV_PERSIST;
  const char *whatstr[] = { "none", "IN", "OUT", "INOUT", "REMOVE" };

  TRACE("%p %d Changing action from %s to %s",
        sinfo, sock, whatstr[sinfo->action], whatstr[act]);

  if (act & CURL_POLL_IN)
    events |= EV_READ;
  if (act & CURL_POLL_OUT)
    events |= EV_WRITE;

  sinfo->sock = sock;
  sinfo->action = act;
  sinfo->easy = easy;

  if (sinfo->ev)
    event_free(sinfo->ev);
  sinfo->ev = event_new(ginfo->evbase, sinfo->sock, events, event_cb, ginfo);
  event_add(sinfo->ev, NULL);
  return 0;
}

/* Initialize a new SockInfo structure */
static int
add_sock(curl_socket_t sock,
         CURL *easy,
         int action,
         GlobalInfo_t *ginfo)
{
  SockInfo_t *sinfo = calloc(1, sizeof(*sinfo));
  int ret = 0;

  if (sinfo) {
    CURLMcode rc ;

    sinfo->global = ginfo;
    sinfo->timeout = 3 * 1000 * 1000;
    set_sock(sinfo, sock, easy, action, ginfo);

    rc = curl_multi_assign(ginfo->multi, sock, sinfo);
    if (rc != CURLM_OK) {
      TRACE("ERROR: curl_multi_assign in %s returns %s",
            __func__, curl_multi_strerror(rc));
      free(sinfo);
      ret = -1;
    }
  }
  return ret;
}


/* CURLMOPT_SOCKETFUNCTION */
static int
sock_update(CURL *easy,
            curl_socket_t sock,
            int what,
            void *multi_arg,
            void *easy_arg)
{
  GlobalInfo_t *ginfo = multi_arg;
  SockInfo_t *sinfo = easy_arg;
  int ret = 0;

  TRACE("easy:%p sock:%d sinfo:%p what:%d", easy, sock, sinfo, what);

  if (what == CURL_POLL_REMOVE) {
    rm_sock(sinfo);
  } else {
    if (sinfo) {
      set_sock(sinfo, sock, easy, what, ginfo);
    } else {
      ret = add_sock(sock, easy, what, ginfo);
    }
  }
  return ret;
}


/* CURLOPT_WRITEFUNCTION */
static size_t
write_cb(void *ptr __attribute__((unused)),
         size_t size,
         size_t nmemb,
         void *data)
{
  size_t realsize = size * nmemb;
  ConnInfo_t *cinfo = data;

  TRACE("%p", cinfo);

  return realsize;
}


/* CURLOPT_PROGRESSFUNCTION */
static inline int
prog_cb(void *p,
        double dltotal,
        double dlnow,
        double ult __attribute__((unused)),
        double uln __attribute__((unused)))
{
  ConnInfo_t *cinfo = p;

  TRACE("Progress: %s (%g/%g)", cinfo->url, dlnow, dltotal);
  return 0;
}


#define EASY_SETOPT(_e, _o, _p) {                       \
    CURLcode _rc = curl_easy_setopt((_e), (_o), (_p));  \
    if (_rc != CURLE_OK) {                              \
      TRACE("ERROR: curl_easy_setopt in %s returns %s", \
            __func__, curl_easy_strerror(_rc));         \
    }                                                   \
  }

/* Create a new easy handle, and add it to the global curl_multi */
static void
new_conn(const char *url,
         GlobalInfo_t *ginfo)
{
  ConnInfo_t *cinfo;

  cinfo = calloc(1, sizeof(*cinfo));
  cinfo->error[0]='\0';

  cinfo->easy = curl_easy_init();
  if (!cinfo->easy) {
    TRACE("curl_easy_init() failed, exiting!");
    exit(2);
  }

  cinfo->global = ginfo;
  cinfo->url = strdup(url);

  EASY_SETOPT(cinfo->easy, CURLOPT_URL, cinfo->url);

  EASY_SETOPT(cinfo->easy, CURLOPT_WRITEFUNCTION, write_cb);
  EASY_SETOPT(cinfo->easy, CURLOPT_WRITEDATA, &cinfo);

  EASY_SETOPT(cinfo->easy, CURLOPT_VERBOSE, 1L);
  EASY_SETOPT(cinfo->easy, CURLOPT_ERRORBUFFER, cinfo->error);

  EASY_SETOPT(cinfo->easy, CURLOPT_PRIVATE, cinfo);
  EASY_SETOPT(cinfo->easy, CURLOPT_NOPROGRESS, 0L);

  //  EASY_SETOPT(cinfo->easy, CURLOPT_PROGRESSFUNCTION, prog_cb);
  //  EASY_SETOPT(cinfo->easy, CURLOPT_PROGRESSDATA, cinfo);

  TRACE("Adding easy %p to multi %p (%s)", cinfo->easy, ginfo->multi, url);
  if (MULTI_ADD_EASY(ginfo->multi, cinfo->easy))
    exit(1);

  /* note that the add_handle() will set a time-out to trigger very soon so
     that the necessary socket_action() call will be called by this app */
}

/*****************************************************************************
 * FIFO
 *****************************************************************************/
/* This gets called whenever data is received from the fifo */
static void
fifo_cb(int fd __attribute__((unused)),
        short events __attribute__((unused)),
        void *arg)
{
  char s[1024];
  long int rv = 0;
  int n = 0;
  GlobalInfo_t *ginfo = arg;

  do {
    s[0] = '\0';
    rv = fscanf(ginfo->input, "%1023s%n", s, &n);
    s[n] = '\0';
    if (n && s[0]) {
      new_conn(s, ginfo);  /* if we read a URL, go get it! */
    } else
      break;
  } while (rv != EOF);
}


/* Create a named pipe and tell libevent to monitor it */
static const char *fifo = "hiper.fifo";

static int
init_fifo(GlobalInfo_t *ginfo)
{
  struct stat st;
  curl_socket_t sockfd;

  TRACE("Creating named pipe \"%s\"", fifo);
  if (lstat(fifo, &st) == 0) {
    if ((st.st_mode & S_IFMT) == S_IFREG) {
      errno = EEXIST;
      perror("lstat");
      return -1;
    }
  }

  unlink(fifo);
  if (mkfifo(fifo, 0600) == -1) {
    perror("mkfifo");
    return -1;
  }

  sockfd = open(fifo, O_RDWR | O_NONBLOCK, 0);
  if (sockfd == -1) {
    perror("open");
    return -1;
  }
  ginfo->input = fdopen(sockfd, "r");

  TRACE("Now, pipe some URL's into > %s", fifo);
  ginfo->fifo_ev = event_new(ginfo->evbase, sockfd, (EV_READ | EV_PERSIST),
                             fifo_cb, ginfo);
  event_add(ginfo->fifo_ev, NULL);
  return 0;
}


static void
clean_fifo(GlobalInfo_t *ginfo)
{
  if (ginfo->fifo_ev) {
    event_free(ginfo->fifo_ev);
    ginfo->fifo_ev = NULL;
  }

  if (ginfo->input) {
    fclose(ginfo->input);
    ginfo->input = NULL;
  }

  unlink(fifo);
}


/*****************************************************************************
 * main
 *****************************************************************************/
int
main(void)
{
  GlobalInfo_t *ginfo;

  //  unsetenv("http_proxy");

  ginfo = calloc(1, sizeof(*ginfo));
  ginfo->evbase = event_base_new();

  if (init_fifo(ginfo))
    exit(1);

  ginfo->multi = curl_multi_init();
  ginfo->timer_ev = evtimer_new(ginfo->evbase, multi_timer_cb, ginfo);

  /* setup the generic multi interface options we want */
  curl_multi_setopt(ginfo->multi, CURLMOPT_SOCKETFUNCTION, sock_update);
  curl_multi_setopt(ginfo->multi, CURLMOPT_SOCKETDATA, ginfo);

  curl_multi_setopt(ginfo->multi, CURLMOPT_TIMERFUNCTION, multi_timer_update);
  curl_multi_setopt(ginfo->multi, CURLMOPT_TIMERDATA, ginfo);

  /* we don't call any curl_multi_socket*() function yet as we have no handles
     added! */

  event_base_dispatch(ginfo->evbase);

  /* this, of course, won't get called since only way to stop this program is
     via ctrl-C, but it is here to show how cleanup /would/ be done. */
  clean_fifo(ginfo);
  event_free(ginfo->timer_ev);
  event_base_free(ginfo->evbase);
  curl_multi_cleanup(ginfo->multi);

  free(ginfo);
  return 0;
}
