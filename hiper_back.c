/* Called by libevent when we get action on a multi socket */
static void
event_cb(int fd,
         short events,
         void *arg)
{
  GlobalInfo_t *ginfo = arg;
  int action = 0;

  TRACE("%p 0x%x", ginfo, events);

  action = event2action(events);
  if (MULTI_ACTION(ginfo->multi, fd, action, &ginfo->running))
    exit(1);

  clean_completed_transaction(ginfo);

  if (ginfo->running <= 0) {
    if (evtimer_pending(ginfo->timer_ev, NULL)) {
      TRACE("last transfer done, kill timeout");
      evtimer_del(ginfo->timer_ev);
    }
  }
}
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
         int action,
         GlobalInfo_t *ginfo)
{
  short events = EV_PERSIST;
  const char *whatstr[] = { "none", "IN", "OUT", "INOUT", "REMOVE" };

  TRACE("%p %d Changing action from %s to %s",
        sinfo, sock, whatstr[sinfo->action], whatstr[action]);

  events = action2event(action);
  sinfo->sock = sock;
  sinfo->action = action;
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
static inline int
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



/* Create a new easy handle, and add it to the global curl_multi */
static inline void
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
  return;

 error:
  exit(1);
}
