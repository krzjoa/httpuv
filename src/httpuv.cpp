#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <map>
#include <iomanip>
#include <signal.h>
#include <errno.h>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <uv.h>
#include <base64.hpp>
#include "uvutil.h"
#include "webapplication.h"
#include "http.h"
#include "callbackqueue.h"
#include "utils.h"
#include "debug.h"
#include "httpuv.h"
#include <Rinternals.h>


void throwError(int err,
  const std::string& prefix = std::string(),
  const std::string& suffix = std::string())
{
  ASSERT_MAIN_THREAD()
  std::string msg = prefix + uv_strerror(err) + suffix;
  throw Rcpp::exception(msg.c_str());
}

// For keeping track of all running server apps.
std::vector<uv_stream_t*> pServers;

// ============================================================================
// Background thread and I/O event loop
// ============================================================================

// A queue of tasks to run on the background thread. This is how the main
// thread schedules work to be done on the background thread.
CallbackQueue* background_queue;

uv_thread_t io_thread_id;
bool io_thread_running = false;

uv_async_t async_stop_io_loop;

// The uv loop that we'll use. Should be accessed via get_io_loop().
uv_loop_t io_loop;
bool io_loop_initialized = false;

uv_loop_t* get_io_loop() {
  if (!io_loop_initialized) {
    throw std::runtime_error("io_loop not initialized!");
  }
  return &io_loop;
}

void ensure_io_loop() {
  ASSERT_MAIN_THREAD()
  if (!io_loop_initialized) {
    uv_loop_init(&io_loop);
    io_loop_initialized = true;
  }
}

void close_handle_cb(uv_handle_t* handle, void* arg) {
  ASSERT_BACKGROUND_THREAD()
  uv_close(handle, NULL);
}

void stop_io_loop(uv_async_t *handle) {
  ASSERT_BACKGROUND_THREAD()
  trace("stop_io_loop");
  uv_stop(get_io_loop());
}

void io_thread(void* data) {
  REGISTER_BACKGROUND_THREAD()
  io_thread_running = true;

  // Set up async communication channels
  uv_async_init(get_io_loop(), &async_stop_io_loop, stop_io_loop);

  // Run io_loop. When it stops, this fuction continues and the thread exits.
  uv_run(get_io_loop(), UV_RUN_DEFAULT);

  trace("io_loop stopped");

  // Cleanup stuff
  uv_walk(get_io_loop(), close_handle_cb, NULL);
  uv_run(get_io_loop(), UV_RUN_ONCE);
  uv_loop_close(get_io_loop());
  io_loop_initialized = false;
}

void ensure_io_thread() {
  ASSERT_MAIN_THREAD()
  if (io_thread_running) {
    return;
  }

  ensure_io_loop();
  background_queue = new CallbackQueue(get_io_loop());

  // TODO: pass data?
  int ret = uv_thread_create(&io_thread_id, io_thread, NULL);

  if (ret != 0) {
    Rcpp::stop(std::string("Error: ") + uv_strerror(ret));
  }
}


// ============================================================================
// Outgoing websocket messages
// ============================================================================

// [[Rcpp::export]]
void sendWSMessage(std::string conn, bool binary, Rcpp::RObject message) {
  ASSERT_MAIN_THREAD()
  WebSocketConnection* wsc = internalize<WebSocketConnection>(conn);

  Opcode mode;
  SEXP msg_sexp;
  std::vector<char>* str;

  // Efficiently copy message into a new vector<char>. There's probably a
  // cleaner way to do this.
   if (binary) {
    mode = Binary;
    msg_sexp = PROTECT(Rcpp::as<SEXP>(message));
    str = new std::vector<char>(RAW(msg_sexp), RAW(msg_sexp) + Rf_length(msg_sexp));
    UNPROTECT(1);

  } else {
    mode = Text;
    msg_sexp = PROTECT(STRING_ELT(message, 0));
    str = new std::vector<char>(CHAR(msg_sexp), CHAR(msg_sexp) + Rf_length(msg_sexp));
    UNPROTECT(1);
  }


  boost::function<void (void)> cb(
    boost::bind(&WebSocketConnection::sendWSMessage, wsc,
      mode,
      &(*str)[0],
      str->size()
    )
  );

  background_queue->push(cb);
  // Free str after data is written
  // delete_cb<std::vector<char>*>(str)
  background_queue->push(boost::bind(delete_cb_bg<std::vector<char>*>, str));
}

// [[Rcpp::export]]
void closeWS(std::string conn) {
  ASSERT_MAIN_THREAD()
  WebSocketConnection* wsc = internalize<WebSocketConnection>(conn);

  // Schedule on background thread:
  // wsc->closeWS();
  background_queue->push(
    boost::bind(&WebSocketConnection::closeWS, wsc)
  );
}


// [[Rcpp::export]]
Rcpp::RObject makeTcpServer(const std::string& host, int port,
                            Rcpp::Function onHeaders,
                            Rcpp::Function onBodyData,
                            Rcpp::Function onRequest,
                            Rcpp::Function onWSOpen,
                            Rcpp::Function onWSMessage,
                            Rcpp::Function onWSClose) {

  using namespace Rcpp;
  REGISTER_MAIN_THREAD()

  // Deleted when owning pServer is deleted. If pServer creation fails,
  // it's still createTcpServer's responsibility to delete pHandler.
  RWebApplication* pHandler =
    new RWebApplication(onHeaders, onBodyData, onRequest, onWSOpen,
                        onWSMessage, onWSClose);

  ensure_io_thread();

  uv_barrier_t blocker;
  uv_barrier_init(&blocker, 2);

  uv_stream_t* pServer;

  // Run on background thread:
  // createTcpServerSync(
  //   get_io_loop(), host.c_str(), port, (WebApplication*)pHandler,
  //   background_queue, &pServer, &blocker
  // );
  background_queue->push(
    boost::bind(createTcpServerSync,
      get_io_loop(), host.c_str(), port, (WebApplication*)pHandler,
      background_queue, &pServer, &blocker
    )
  );

  // Wait for server to be created before continuing
  uv_barrier_wait(&blocker);

  if (!pServer) {
    return R_NilValue;
  }

  pServers.push_back(pServer);

  return Rcpp::wrap(externalize<uv_stream_t>(pServer));
}

// [[Rcpp::export]]
Rcpp::RObject makePipeServer(const std::string& name,
                             int mask,
                             Rcpp::Function onHeaders,
                             Rcpp::Function onBodyData,
                             Rcpp::Function onRequest,
                             Rcpp::Function onWSOpen,
                             Rcpp::Function onWSMessage,
                             Rcpp::Function onWSClose) {

  using namespace Rcpp;
  REGISTER_MAIN_THREAD()

  // Deleted when owning pServer is deleted. If pServer creation fails,
  // it's still createTcpServer's responsibility to delete pHandler.
  RWebApplication* pHandler =
    new RWebApplication(onHeaders, onBodyData, onRequest, onWSOpen,
                        onWSMessage, onWSClose);

  ensure_io_thread();

  uv_barrier_t blocker;
  uv_barrier_init(&blocker, 2);

  uv_stream_t* pServer;

  // Run on background thread:
  // createPipeServerSync(
  //   get_io_loop(), name.c_str(), mask, (WebApplication*)pHandler,
  //   background_queue, &pServer, &blocker
  // );
  background_queue->push(
    boost::bind(createPipeServerSync,
      get_io_loop(), name.c_str(), mask, (WebApplication*)pHandler,
      background_queue, &pServer, &blocker
    )
  );

  // Wait for server to be created before continuing
  uv_barrier_wait(&blocker);

  if (!pServer) {
    return R_NilValue;
  }

  pServers.push_back(pServer);

  return Rcpp::wrap(externalize<uv_stream_t>(pServer));
}


void stopServer(uv_stream_t* pServer) {
  ASSERT_MAIN_THREAD()

  // Remove it from the list of running servers.
  // Note: we're removing it from the pServers list without waiting for the
  // background thread to call freeServer().
  std::vector<uv_stream_t*>::iterator pos = std::find(pServers.begin(), pServers.end(), pServer);
  if (pos != pServers.end()) {
    pServers.erase(pos);
  } else {
    throw Rcpp::exception("pServer handle not found in list of running servers.");
  }

  // Run on background thread:
  // freeServer(pServer);
  background_queue->push(
    boost::bind(freeServer, pServer)
  );
}

//' Stop a running server
//' 
//' Given a handle that was returned from a previous invocation of 
//' \code{\link{startServer}}, closes all open connections for that server and 
//' unbinds the port. \strong{Be careful not to call \code{stopServer} more than 
//' once on a handle, as this will cause the R process to crash!}
//' 
//' @param handle A handle that was previously returned from
//'   \code{\link{startServer}}.
//'   
//' @export
// [[Rcpp::export]]
void stopServer(std::string handle) {
  ASSERT_MAIN_THREAD()
  uv_stream_t* pServer = internalize<uv_stream_t>(handle);
  stopServer(pServer);
}

// [[Rcpp::export]]
void stopAllServers() {
  ASSERT_MAIN_THREAD()

  if (!io_thread_running)
    return;

  // Each call to stopServer also removes it from the pServers list.
  while (pServers.size() > 0) {
    stopServer(pServers[0]);
  }

  uv_async_send(&async_stop_io_loop);

  uv_thread_join(&io_thread_id);
  io_thread_running = false;
}

void stop_loop_timer_cb(uv_timer_t* handle) {
  uv_stop(handle->loop);
}


// ============================================================================
// Miscellaneous utility functions
// ============================================================================

// [[Rcpp::export]]
std::string base64encode(const Rcpp::RawVector& x) {
  return b64encode(x.begin(), x.end());
}

static std::string allowed = ";,/?:@&=+$abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890-_.!~*'()";

bool isReservedUrlChar(char c) {
  switch (c) {
    case ';':
    case ',':
    case '/':
    case '?':
    case ':':
    case '@':
    case '&':
    case '=':
    case '+':
    case '$':
      return true;
    default:
      return false;
  }
}

bool needsEscape(char c, bool encodeReserved) {
  if (c >= 'a' && c <= 'z')
    return false;
  if (c >= 'A' && c <= 'Z')
    return false;
  if (c >= '0' && c <= '9')
    return false;
  if (isReservedUrlChar(c))
    return encodeReserved;
  switch (c) {
    case '-':
    case '_':
    case '.':
    case '!':
    case '~':
    case '*':
    case '\'':
    case '(':
    case ')':
      return false;
  }
  return true;
}

std::string doEncodeURI(std::string value, bool encodeReserved) {
  std::ostringstream os;
  os << std::hex << std::uppercase;
  for (std::string::const_iterator it = value.begin();
    it != value.end();
    it++) {
    
    if (!needsEscape(*it, encodeReserved)) {
      os << *it;
    } else {
      os << '%' << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(*it));
    }
  }
  return os.str();
}

//' URI encoding/decoding
//' 
//' Encodes/decodes strings using URI encoding/decoding in the same way that web
//' browsers do. The precise behaviors of these functions can be found at
//' developer.mozilla.org:
//' \href{https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/encodeURI}{encodeURI},
//' \href{https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/encodeURIComponent}{encodeURIComponent},
//' \href{https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/decodeURI}{decodeURI},
//' \href{https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/decodeURIComponent}{decodeURIComponent}
//' 
//' Intended as a faster replacement for \code{\link[utils]{URLencode}} and
//' \code{\link[utils]{URLdecode}}.
//' 
//' encodeURI differs from encodeURIComponent in that the former will not encode
//' reserved characters: \code{;,/?:@@&=+$}
//' 
//' decodeURI differs from decodeURIComponent in that it will refuse to decode
//' encoded sequences that decode to a reserved character. (If in doubt, use
//' decodeURIComponent.)
//' 
//' The only way these functions differ from web browsers is in the encoding of
//' non-ASCII characters. All non-ASCII characters will be escaped byte-by-byte.
//' If conformant non-ASCII behavior is important, ensure that your input vector
//' is UTF-8 encoded before calling encodeURI or encodeURIComponent.
//' 
//' @param value Character vector to be encoded or decoded.
//' @return Encoded or decoded character vector of the same length as the
//'   input value.
//'
//' @export
// [[Rcpp::export]]
std::vector<std::string> encodeURI(std::vector<std::string> value) {
  for (std::vector<std::string>::iterator it = value.begin();
    it != value.end();
    it++) {

    *it = doEncodeURI(*it, false);
  }
  
  return value;
}

//' @rdname encodeURI
//' @export
// [[Rcpp::export]]
std::vector<std::string> encodeURIComponent(std::vector<std::string> value) {
  for (std::vector<std::string>::iterator it = value.begin();
    it != value.end();
    it++) {

    *it = doEncodeURI(*it, true);
  }
  
  return value;
}

int hexToInt(char c) {
  switch (c) {
    case '0': return 0;
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'A': case 'a': return 10;
    case 'B': case 'b': return 11;
    case 'C': case 'c': return 12;
    case 'D': case 'd': return 13;
    case 'E': case 'e': return 14;
    case 'F': case 'f': return 15;
    default: return -1;
  }
}

std::string doDecodeURI(std::string value, bool component) {
  std::ostringstream os;
  for (std::string::const_iterator it = value.begin();
    it != value.end();
    it++) {
    
    // If there aren't enough characters left for this to be a
    // valid escape code, just use the character and move on
    if (it > value.end() - 3) {
      os << *it;
      continue;
    }
    
    if (*it == '%') {
      char hi = *(++it);
      char lo = *(++it);
      int iHi = hexToInt(hi);
      int iLo = hexToInt(lo);
      if (iHi < 0 || iLo < 0) {
        // Invalid escape sequence
        os << '%' << hi << lo;
        continue;
      }
      char c = (char)(iHi << 4 | iLo);
      if (!component && isReservedUrlChar(c)) {
        os << '%' << hi << lo;
      } else {
        os << c;
      }
    } else {
      os << *it;
    }
  }
  
  return os.str();
}

//' @rdname encodeURI
//' @export
// [[Rcpp::export]]
std::vector<std::string> decodeURI(std::vector<std::string> value) {
  for (std::vector<std::string>::iterator it = value.begin();
    it != value.end();
    it++) {

    *it = doDecodeURI(*it, false);
  }
  
  return value;
}

//' @rdname encodeURI
//' @export
// [[Rcpp::export]]
std::vector<std::string> decodeURIComponent(std::vector<std::string> value) {
  for (std::vector<std::string>::iterator it = value.begin();
    it != value.end();
    it++) {

    *it = doDecodeURI(*it, true);
  }
  
  return value;
}

// Given a List and an external pointer to a C++ function that takes a List,
// invoke the function with the List as the single argument. This also clears
// the external pointer so that the C++ function can't be called again.
// [[Rcpp::export]]
void invokeCppCallback(Rcpp::List data, SEXP callback_xptr) {
  ASSERT_MAIN_THREAD()

  if (TYPEOF(callback_xptr) != EXTPTRSXP) {
     throw Rcpp::exception("Expected external pointer.");
  }
  boost::function<void(Rcpp::List)>* callback_wrapper =
    (boost::function<void(Rcpp::List)>*)(R_ExternalPtrAddr(callback_xptr));

  (*callback_wrapper)(data);

  // We want to clear the external pointer to make sure that the C++ function
  // can't get called again by accident. Also delete the heap-allocated
  // boost::function.
  delete callback_wrapper;
  R_ClearExternalPtr(callback_xptr);
}

//' Apply the value of .Random.seed to R's internal RNG state
//'
//' This function is needed in unusual cases where a C++ function calls
//' an R function which sets the value of \code{.Random.seed}. This function
//' should be called at the end of the R function to ensure that the new value
//' \code{.Random.seed} is preserved. Otherwise, Rcpp may overwrite it with a
//' previous value.
//'
//' @keywords internal
//' @export
// [[Rcpp::export]]
void getRNGState() {
  GetRNGstate();
}
