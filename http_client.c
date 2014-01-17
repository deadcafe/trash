#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <unistd.h>
#include <curl/curl.h>

#include <trema.h>

#include "queue.h"
#include "http_client.h"
#include "bd_fifo.h"


#define USER_AGENT "Bisco/0.0.3"


typedef struct {
  uint8_t method;
  char uri[1024];
  http_content content;
} request;


typedef struct {
  int status;
  int code;
  http_content content;
} response;


typedef struct {
  CURL *handle;

  struct curl_slist *slist;
  request req;
  response res;

  request_completed_handler cb;
  void *arg;
} transaction;


typedef struct _GlobalInfo
{
  hash_table *db;

  CURLM *multi;
  int still_running;
  bd_fifo_t *th_fifo;
} GlobalInfo;


typedef struct _ConnInfo
{
  CURL *easy;
  char *url;
  GlobalInfo *global;
  char error[CURL_ERROR_SIZE];
} ConnInfo;

typedef struct _SockInfo
{
  curl_socket_t sockfd;
  CURL *easy;
  int action;
  long timeout;
  int evset;
  GlobalInfo *global;
} SockInfo;

static pthread_t client_thread;


static bool
compare_transaction( const void *x, const void *y ) {
  const transaction *trans_x = x;
  const transaction *trans_y = y;

  return ( trans_x->handle == trans_y->handle ) ? true : false;
}


static unsigned int
hash_transaction( const void *key ) {
  return ( unsigned int ) ( uintptr_t ) key;
}

static transaction *
alloc_transaction(void) {
  transaction *trans = xmalloc( sizeof( *trans ) );
  memset( trans, 0, sizeof( *trans ) );
  return trans;
}



static void
free_transaction( transaction *trans ) {
  if ( trans->handle != NULL ) {
    curl_easy_cleanup( trans->handle );
  }
  if ( transaction->slist != NULL ) {
    curl_slist_free_all( trans->slist );
  }
  if ( trans->req.content.body != NULL ) {
    free_buffer( trans->req.content.body );
  }
  if ( trans->res.content.body != NULL ) {
    free_buffer( trans->res.content.body );
  }
  xfree( trans );
}


static bool
add_transaction( transaction *trans, CURL *handle ) {
  trans->handle = handle;
  CURLMcode ret = curl_multi_add_handle( curl_handle, trans->handle );
  if ( ret != CURLM_OK && ret != CURLM_CALL_MULTI_PERFORM ) {
    error( "Failed to add an easy handle to a multi handle ( curl_handle = %p, handle = %p, error = %s ).",
           curl_handle, transaction->handle, curl_multi_strerror( ret ) );
    return false;
  }
  ret = curl_multi_perform( curl_handle, &curl_running );
  if ( ret != CURLM_OK && ret != CURLM_CALL_MULTI_PERFORM ) {
    error( "Failed to run a multi handle ( curl_handle = %p, error = %s ).", curl_handle, curl_multi_strerror( ret ) );
    ret = curl_multi_remove_handle( curl_handle, transaction->handle );
    if ( ret != CURLM_OK  && ret != CURLM_CALL_MULTI_PERFORM ) {
      error( "Failed to remove an easy handle from a multi handle ( curl_handle = %p, handle = %p, error = %s ).",
             curl_handle, transaction->handle, curl_multi_strerror( ret ) );
    }
    return false;
  }

  void *duplicated = insert_hash_entry( transactions, transaction->handle, transaction );
  if ( duplicated != NULL ) {
    // FIXME: handle duplication properly
    warn( "Duplicated HTTP transaction found ( %p ).", duplicated );
    free_transaction( duplicated );
  }

  return true;
}


static bool
delete_transaction( transaction *trans ) {
  void *deleted = delete_hash_entry( trans, trans->handle );
  if ( deleted == NULL ) {
    return false;
  }

  CURLMcode ret = curl_multi_remove_handle( curl_handle, trans->handle );
  if ( ret != CURLM_OK  && ret != CURLM_CALL_MULTI_PERFORM ) {
    error( "Failed to remove an easy handle from a multi handle ( curl_handle = %p, handle = %p, error = %s ).",
           curl_handle, transaction->handle, curl_multi_strerror( ret ) );
  }
  if ( trans->slist != NULL ) {
    curl_slist_free_all( trans->slist );
    trans->slist = NULL;
  }
  curl_easy_cleanup( trans->handle );
  trans->handle = NULL;

  return true;
}


static transaction *
lookup_transaction( GlobalInfo *g, CURL *easy_handle ) {
  transaction *trans = lookup_hash_entry( g->db, easy_handle );

  return trans;
}


static void
create_transaction_db(GlobalInfo *g) {
  g->db = create_hash( compare_transaction, hash_transaction );
}


static void
destroy_transaction_db(GlobalInfo *g) {
  hash_entry *e = NULL;
  hash_iterator iter;

  init_hash_iterator( g->db, &iter );
  while ( ( e = iterate_hash_next( &iter ) ) != NULL ) {
    if ( e->value != NULL ) {
      free_transaction( e->value );
      e->value = NULL;
    }
  }

  delete_hash( g->db );
  g->db = NULL;
}


static size_t
write_to_buffer( void *contents, size_t size, size_t nmemb, void *user_data ) {
  size_t real_size = size * nmemb;
  buffer *buf = user_data;

  if (real_size) {
    void *p = append_back_buffer( buf, real_size );
    memcpy( p, contents, real_size );
  }
  return real_size;
}


static size_t
read_from_buffer( void *ptr, size_t size, size_t nmemb, void *user_data ) {
  buffer *buf = user_data;
  size_t real_size = size * nmemb;

  if ( buf == NULL || ( buf != NULL && buf->length == 0 ) ) {
    return 0;
  }

  if ( real_size ) {
    if ( buf->length < real_size ) {
      real_size = buf->length;
    }
    memcpy( ptr, buf->data, real_size );
    remove_front_buffer( buf, real_size );
  }
  return real_size;
}


static bool
set_contents( CURL *handle, transaction *trans ) {
  request *req = &trans->req;

  if ( req->content.body == NULL ||
       ( req->content.body != NULL && req->content.body->length == 0 ) ) {
    return true;
  }

  if ( strlen( req->content.content_type ) > 0 ) {
    char content_type[ 128 ];
    memset( content_type, '\0', sizeof( content_type ) );
    snprintf( content_type, sizeof( content_type ), "Content-Type: %s",
              req->content.content_type );
    trans->slist = curl_slist_append( trans->slist, content_type );
  }
  else {
    trans->slist = curl_slist_append( trans->slist,
                                      "Content-Type: text/plain" );
  }

  CURLcode ret = curl_easy_setopt( handle, CURLOPT_READFUNCTION, read_from_buffer );
  if ( ret != CURLE_OK ) {
    error( "Failed READFUNC ( handle = %p, error = %s ).",
           handle, curl_easy_strerror( ret ) );
    return false;
  }
  ret = curl_easy_setopt( handle, CURLOPT_READDATA, req->content.body );
  if ( ret != CURLE_OK ) {
    error( "Failed READDATA ( handle = %p, error = %s ).",
           handle, curl_easy_strerror( ret ) );
    return false;
  }
  return true;
}


static void
transaction_from_main( transaction *trans ) {
  request *req = &trans->req;
  uint32_t content_length = 0;

  if ( req->content.body != NULL && req->content.body->length > 0 ) {
    content_length = ( uint32_t ) --req->content.body->length; // We only accept null-terminated string.
  }

  trans->slist = NULL;
  CURL *handle = curl_easy_init();
  CURLcode ret = CURLE_OK;

  switch ( req->method ) {
    case HTTP_METHOD_GET:
    {
      ret = curl_easy_setopt( handle, CURLOPT_HTTPGET, 1 );
      if ( ret != CURLE_OK ) {
        error( "Failed HTTP GET ( handle = %p, error = %s ).",
               handle, curl_easy_strerror( retval ) );
        goto error;
      }
    }
    break;

    case HTTP_METHOD_PUT:
    {
      ret = curl_easy_setopt( handle, CURLOPT_UPLOAD, 1 );
      if ( ret != CURLE_OK ) {
        error( "Failed HTTP PUT ( handle = %p, error = %s ).",
               handle, curl_easy_strerror( retval ) );
        goto error;
      }
      ret = curl_easy_setopt( handle, CURLOPT_INFILESIZE, content_length );
      if ( ret != CURLE_OK ) {
        error( "Failed length ( content_length = %u, handle = %p, error = %s ).",
               content_length, handle, curl_easy_strerror( ret ) );
        goto error;
      }
      trans->slist = curl_slist_append( trans->slist, "Expect:" );
    }
    break;

    case HTTP_METHOD_POST:
    {
      ret = curl_easy_setopt( handle, CURLOPT_POST, 1 );
      if ( ret != CURLE_OK ) {
        error( "Failed HTTP POST ( handle = %p, error = %s ).",
               handle, curl_easy_strerror( ret ) );
        goto error;
      }
      ret = curl_easy_setopt( handle, CURLOPT_POSTFIELDSIZE, content_length );
      if ( ret != CURLE_OK ) {
        error( "Failed content length value ( content_length = %u, handle = %p, error = %s ).",
               content_length, handle, curl_easy_strerror( ret ) );
        goto error;
      }
    }
    break;

    case HTTP_METHOD_DELETE:
    {
      ret = curl_easy_setopt( handle, CURLOPT_CUSTOMREQUEST, "DELETE" );
      if ( ret != CURLE_OK ) {
        error( "Failed HTTP DELETE ( handle = %p, error = %s ).",
               handle, curl_easy_strerror( ret ) );
        goto error;
      }
    }
    break;

    default:
    {
      error( "Undefined HTTP request method ( %u ).", req->method );
      assert( 0 );
    }
    break;
  }

  if (!set_contents( handle, trans )) {
    goto error;
  }

  ret = curl_easy_setopt( handle, CURLOPT_USERAGENT, USER_AGENT );
  if ( ret != CURLE_OK ) {
    error( "Failed User-Agent ( handle = %p, transaction = %p, error = %s ).",
           handle, trans, curl_easy_strerror( ret ) );
    goto error;
  }
  ret = curl_easy_setopt( handle, CURLOPT_URL, req->uri );
  if ( ret != CURLE_OK ) {
    error( "Failed URL ( uri = %s, handle = %p, transaction = %p, error = %s ).",
           req->uri, handle, transaction, curl_easy_strerror( ret ) );
    goto error;
  }
  ret = curl_easy_setopt( handle, CURLOPT_NOPROGRESS, 1 );
  if ( ret != CURLE_OK ) {
    error( "Failed NOPROGRESS ( handle = %p, transaction = %p, error = %s ).",
           handle, trans, curl_easy_strerror( ret ) );
    goto error;
  }
  ret = curl_easy_setopt( handle, CURLOPT_WRITEFUNCTION, write_to_buffer );
  if ( ret != CURLE_OK ) {
    error( "Failed WRITE FUNC ( handle = %p, transaction = %p, error = %s ).",
           handle, trans, curl_easy_strerror( ret ) );
    goto error;
  }

  response *res = &trans->res;
  res->content.body = alloc_buffer_with_length( 1024 );
  ret = curl_easy_setopt( handle, CURLOPT_WRITEDATA, res->content.body );
  if ( ret != CURLE_OK ) {
    error( "Failed WRITE DATA ( handle = %p, transaction = %p, error = %s ).",
           handle, trans, curl_easy_strerror( ret ) );
    goto error;
  }
  if ( trans->slist != NULL ) {
    ret = curl_easy_setopt( handle, CURLOPT_HTTPHEADER, trans->slist );
    if ( ret != CURLE_OK ) {
      error( "Failed custom header ( handle = %p, transaction = %p, slist = %p, error = %s ).",
             handle, trans, trans->slist, curl_easy_strerror( ret ) );
      goto error;
    }
  }

  add_transaction( trans, handle );
  return;

error:
  if ( handle != NULL ) {
    curl_easy_cleanup( handle );
  }
  if ( trans->slist != NULL ) {
    curl_slist_free_all( trans->slist );
    trans->slist = NULL;
  }
  res = &trans->res;
  res->status = TRANSACTION_FAILED;
  res->code = 0;
  fifo_to_client( http_client_res, trans );
}


/***********************************************************************************
 * HTTP client thread context funcsions
 ***********************************************************************************/
static bool
finalize_curl(GlobalInfo *g) {
  if (g->multi) {
    curl_multi_cleanup( g->multi);
    g->multi = NULL;
  }
  return true;
}


static void
cleanup_cb(void *arg )
{
  GlobalInfo *g = arg;

  finalize_curl(g);
  fifo_detach_server(g->fifo);
  fifo_destroy(fifo);

  finalize_timer_safe();
  finalize_event_handler_safe();
  delete_transaction_db();
  xfree(g);
}

static void *
thread_entry( void *arg )
{
  GlobalInfo *g = arg;

  pthread_cleanup_push(cleanup_cb, g);

  init_event_handler_safe();
  init_timer_safe();
  fifo_bind_server(g->fifo);

  init_curl(g);
  start_event_handler_safe();

  return g;
}

static bool
init_curl(GlobalInfo *g) {
  g->multi = curl_multi_init();
  curl_multi_setopt(g.multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
  curl_multi_setopt(g.multi, CURLMOPT_SOCKETDATA, g);
  curl_multi_setopt(g.multi, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
  curl_multi_setopt(g.multi, CURLMOPT_TIMERDATA, g);
  return true;
}

static void
http_client_req( void *arg )
{
  transaction *trans = arg;

  handle_transaction_from_main( trans );
}

/***********************************************************************************
 * main thread context funcsions
 ***********************************************************************************/
static void
http_client_res( void *arg )
{
  transaction *trans = arg;
  request *req = &trans->req;
  response *res = &trans->res;

  if ( trans->cb ) {
    http_content *content;

    if ( res->content.body != NULL && res->content.body->length > 0 ) {
      content = &res->content;
    } else {
      content = NULL;
    }

    trans->cb( res->status, res->code, content, trans->arg );
  }
  free_transaction( trans );
}

/*
 * from main thread to http thread
 */
bool
do_request( uint8_t method,
                 const char *uri,
                 const http_content *content,
                 request_completed_handler cb,
                 void *arg ) {

  transaction *trans = alloc_transaction();
  request *req = &transaction->req;

  req->method = method;
  memset( req->uri, '\0', sizeof( request->uri ) );
  strncpy( req->uri, uri, sizeof( request->uri ) - 1 );

  if ( content ) {
    memcpy( &req->content, content, sizeof(req->content));
    if ( content->body ) {
      req->content.body = duplicate_buffer( content->body );
    }
  }
  trans->cb = cb;
  trans->arg = arg;

  return fifo_to_server( http_client_req, trans );
}

static bd_fifo_t *th_fifo;

bool
init_http_client( void ) {
  GlobalInfo *g = NULL;

  if ((g = xmalloc(sizeof(*g))) != NULL) {
    memset(g, 0, sizeof(*g));
    if ((g->th_fifo = fifo_create()) == NULL) {
      xfree(g);
      return false;
    }

    if (!create_transaction_db(g)) {
      fifo_destroy(g->fifo);
      xfree(g);
      return false;
    }

  pthread_attr_t attr;
  pthread_attr_init( &attr );
  pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );

  int ret = pthread_create( &http_client_thread, &attr, thread_entry, g );
  if ( ret != 0 ) {
    /* XXX: clean up */
    fifo_destroy(g->fifo);
    xfree(g);
    return false;
  }
  fifo_bind_clent(g->fifo);
  th_fifo = g->fifo;
  return true;
}


bool
finalize_http_client( void ) {
  if (th_fifo) {
    fifo_detach_client(th_fifo);
    th_fifo = NULL;
  }
  if (http_client_thread) {
    pthread_cancel(http_client_thread);
    http_client_thread = 0;
  }
  return true;
}



/*
 * Local variables:
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * End:
 */
