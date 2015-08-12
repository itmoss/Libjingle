/*
 * libjingle
 * Copyright 2008, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif  // HAVE_CONFIG_H

#include <unistd.h>

#if HAVE_OPENSSL_SSL_H

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "talk/base/common.h"
#include "talk/base/logging.h"
#include "talk/base/openssladapter.h"
#include "talk/base/sslroots.h"
#include "talk/base/stringutils.h"

// TODO: Use a nicer abstraction for mutex.

#if defined(WIN32)
  #define MUTEX_TYPE HANDLE
  #define MUTEX_SETUP(x) (x) = CreateMutex(NULL, FALSE, NULL)
  #define MUTEX_CLEANUP(x) CloseHandle(x)
  #define MUTEX_LOCK(x) WaitForSingleObject((x), INFINITE)
  #define MUTEX_UNLOCK(x) ReleaseMutex(x)
  #define THREAD_ID GetCurrentThreadId()
#elif defined(_POSIX_THREADS)
  // _POSIX_THREADS is normally defined in unistd.h if pthreads are available
  // on your platform.
  #define MUTEX_TYPE pthread_mutex_t
  #define MUTEX_SETUP(x) pthread_mutex_init(&(x), NULL)
  #define MUTEX_CLEANUP(x) pthread_mutex_destroy(&(x))
  #define MUTEX_LOCK(x) pthread_mutex_lock(&(x))
  #define MUTEX_UNLOCK(x) pthread_mutex_unlock(&(x))
  #define THREAD_ID pthread_self()
#else
  #error You must define mutex operations appropriate for your platform!
#endif

struct CRYPTO_dynlock_value {
  MUTEX_TYPE mutex;
};

//////////////////////////////////////////////////////////////////////
// SocketBIO
//////////////////////////////////////////////////////////////////////

static int socket_write(BIO* h, const char* buf, int num);
static int socket_read(BIO* h, char* buf, int size);
static int socket_puts(BIO* h, const char* str);
static long socket_ctrl(BIO* h, int cmd, long arg1, void* arg2);
static int socket_new(BIO* h);
static int socket_free(BIO* data);

static BIO_METHOD methods_socket = {
  BIO_TYPE_BIO,
  "socket",
  socket_write,
  socket_read,
  socket_puts,
  0,
  socket_ctrl,
  socket_new,
  socket_free,
  NULL,
};

BIO_METHOD* BIO_s_socket2() { return(&methods_socket); }

BIO* BIO_new_socket(talk_base::AsyncSocket* socket) {
  BIO* ret = BIO_new(BIO_s_socket2());
  if (ret == NULL) {
          return NULL;
  }
  ret->ptr = socket;
  return ret;
}

static int socket_new(BIO* b) {
  b->shutdown = 0;
  b->init = 1;
  b->num = 0; // 1 means socket closed
  b->ptr = 0;
  return 1;
}

static int socket_free(BIO* b) {
  if (b == NULL)
    return 0;
  return 1;
}

static int socket_read(BIO* b, char* out, int outl) {
  if (!out)
    return -1;
  talk_base::AsyncSocket* socket = static_cast<talk_base::AsyncSocket*>(b->ptr);
  BIO_clear_retry_flags(b);
  int result = socket->Recv(out, outl);
  if (result > 0) {
    return result;
  } else if (result == 0) {
    b->num = 1;
  } else if (socket->IsBlocking()) {
    BIO_set_retry_read(b);
  }
  return -1;
}

static int socket_write(BIO* b, const char* in, int inl) {
  if (!in)
    return -1;
  talk_base::AsyncSocket* socket = static_cast<talk_base::AsyncSocket*>(b->ptr);
  BIO_clear_retry_flags(b);
  int result = socket->Send(in, inl);
  if (result > 0) {
    return result;
  } else if (socket->IsBlocking()) {
    BIO_set_retry_write(b);
  }
  return -1;
}

static int socket_puts(BIO* b, const char* str) {
  return socket_write(b, str, strlen(str));
}

static long socket_ctrl(BIO* b, int cmd, long num, void* ptr) {
  UNUSED(num);
  UNUSED(ptr);

  switch (cmd) {
  case BIO_CTRL_RESET:
    return 0;
  case BIO_CTRL_EOF:
    return b->num;
  case BIO_CTRL_WPENDING:
  case BIO_CTRL_PENDING:
    return 0;
  case BIO_CTRL_FLUSH:
    return 1;
  default:
    return 0;
  }
}

/////////////////////////////////////////////////////////////////////////////
// OpenSSLAdapter
/////////////////////////////////////////////////////////////////////////////

namespace talk_base {

// This array will store all of the mutexes available to OpenSSL.
static MUTEX_TYPE* mutex_buf = NULL;

static void locking_function(int mode, int n, const char * file, int line) {
  if (mode & CRYPTO_LOCK) {
    MUTEX_LOCK(mutex_buf[n]);
  } else {
    MUTEX_UNLOCK(mutex_buf[n]);
  }
}

static pthread_t id_function() {
  return THREAD_ID;
}

static CRYPTO_dynlock_value* dyn_create_function(const char* file, int line) {
  CRYPTO_dynlock_value* value = new CRYPTO_dynlock_value;
  if (!value)
    return NULL;
  MUTEX_SETUP(value->mutex);
  return value;
}

static void dyn_lock_function(int mode, CRYPTO_dynlock_value* l,
                              const char* file, int line) {
  if (mode & CRYPTO_LOCK) {
    MUTEX_LOCK(l->mutex);
  } else {
    MUTEX_UNLOCK(l->mutex);
  }
}

static void dyn_destroy_function(CRYPTO_dynlock_value* l,
                                 const char* file, int line) {
  MUTEX_CLEANUP(l->mutex);
  delete l;
}

VerificationCallback OpenSSLAdapter::custom_verify_callback_ = NULL;

bool OpenSSLAdapter::InitializeSSL(VerificationCallback callback) {
  if (!InitializeSSLThread() || !SSL_library_init())
      return false;
  SSL_load_error_strings();
  ERR_load_BIO_strings();
  OpenSSL_add_all_algorithms();
  RAND_poll();
  custom_verify_callback_ = callback;
  return true;
}

bool OpenSSLAdapter::InitializeSSLThread() {
  mutex_buf = new MUTEX_TYPE[CRYPTO_num_locks()];
  if (!mutex_buf)
    return false;
  for (int i = 0; i < CRYPTO_num_locks(); ++i)
    MUTEX_SETUP(mutex_buf[i]);

  // we need to cast our id_function to return an unsigned long -- pthread_t is a pointer
  CRYPTO_set_id_callback((unsigned long (*)())id_function);
  CRYPTO_set_locking_callback(locking_function);
  CRYPTO_set_dynlock_create_callback(dyn_create_function);
  CRYPTO_set_dynlock_lock_callback(dyn_lock_function);
  CRYPTO_set_dynlock_destroy_callback(dyn_destroy_function);
  return true;
}

bool OpenSSLAdapter::CleanupSSL() {
  if (!mutex_buf)
    return false;
  CRYPTO_set_id_callback(NULL);
  CRYPTO_set_locking_callback(NULL);
  CRYPTO_set_dynlock_create_callback(NULL);
  CRYPTO_set_dynlock_lock_callback(NULL);
  CRYPTO_set_dynlock_destroy_callback(NULL);
  for (int i = 0; i < CRYPTO_num_locks(); ++i)
    MUTEX_CLEANUP(mutex_buf[i]);
  delete [] mutex_buf;
  mutex_buf = NULL;
  return true;
}

OpenSSLAdapter::OpenSSLAdapter(AsyncSocket* socket)
  : SSLAdapter(socket),
    state_(SSL_NONE),
    ssl_read_needs_write_(false),
    ssl_write_needs_read_(false),
    restartable_(false),
    ssl_(NULL), ssl_ctx_(NULL),
    custom_verification_succeeded_(false) {
}

OpenSSLAdapter::~OpenSSLAdapter() {
  Cleanup();
}

int
OpenSSLAdapter::StartSSL(const char* hostname, bool restartable) {
  if (state_ != SSL_NONE)
    return -1;

  ssl_host_name_ = hostname;
  restartable_ = restartable;

  if (socket_->GetState() != Socket::CS_CONNECTED) {
    state_ = SSL_WAIT;
    return 0;
  }

  state_ = SSL_CONNECTING;
  if (int err = BeginSSL()) {
    Error("BeginSSL", err, false);
    return err;
  }

  return 0;
}

int
OpenSSLAdapter::BeginSSL() {
  LOG(LS_INFO) << "BeginSSL: " << ssl_host_name_;
  ASSERT(state_ == SSL_CONNECTING);

  int err = 0;
  BIO* bio = NULL;

  // First set up the context
  if (!ssl_ctx_)
    ssl_ctx_ = SetupSSLContext();

  if (!ssl_ctx_) {
    err = -1;
    goto ssl_error;
  }

  bio = BIO_new_socket(static_cast<AsyncSocketAdapter*>(socket_));
  if (!bio) {
    err = -1;
    goto ssl_error;
  }

  ssl_ = SSL_new(ssl_ctx_);
  if (!ssl_) {
    err = -1;
    goto ssl_error;
  }

  SSL_set_app_data(ssl_, this);

  SSL_set_bio(ssl_, bio, bio);
  SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE |
                     SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

  // the SSL object owns the bio now
  bio = NULL;

  // Do the connect
  err = ContinueSSL();
  if (err != 0)
    goto ssl_error;

  return err;

ssl_error:
  Cleanup();
  if (bio)
    BIO_free(bio);

  return err;
}

int
OpenSSLAdapter::ContinueSSL() {
  LOG(LS_INFO) << "ContinueSSL";
  ASSERT(state_ == SSL_CONNECTING);

  int code = SSL_connect(ssl_);
  switch (SSL_get_error(ssl_, code)) {
  case SSL_ERROR_NONE:
    LOG(LS_INFO) << " -- success";

    if (!SSLPostConnectionCheck(ssl_, ssl_host_name_.c_str())) {
      LOG(LS_ERROR) << "TLS post connection check failed";
      // make sure we close the socket
      Cleanup();
      // The connect failed so return -1 to shut down the socket
      return -1;
    }

    state_ = SSL_CONNECTED;
    AsyncSocketAdapter::OnConnectEvent(this);
#if 0  // TODO: worry about this
    // Don't let ourselves go away during the callbacks
    PRefPtr<OpenSSLAdapter> lock(this);
    LOG(LS_INFO) << " -- onStreamReadable";
    AsyncSocketAdapter::OnReadEvent(this);
    LOG(LS_INFO) << " -- onStreamWriteable";
    AsyncSocketAdapter::OnWriteEvent(this);
#endif
    break;

  case SSL_ERROR_WANT_READ:
    LOG(LS_INFO) << " -- error want read";
    break;

  case SSL_ERROR_WANT_WRITE:
    LOG(LS_INFO) << " -- error want write";
    break;

  case SSL_ERROR_ZERO_RETURN:
  default:
    LOG(LS_INFO) << " -- error " << code;
    return (code != 0) ? code : -1;
  }

  return 0;
}

void
OpenSSLAdapter::Error(const char* context, int err, bool signal) {
  LOG(LS_WARNING) << "OpenSSLAdapter::Error("
                  << context << ", " << err << ")";
  state_ = SSL_ERROR;
  SetError(err);
  if (signal)
    AsyncSocketAdapter::OnCloseEvent(this, err);
}

void
OpenSSLAdapter::Cleanup() {
  LOG(LS_INFO) << "Cleanup";

  state_ = SSL_NONE;
  ssl_read_needs_write_ = false;
  ssl_write_needs_read_ = false;
  custom_verification_succeeded_ = false;

  if (ssl_) {
    SSL_free(ssl_);
    ssl_ = NULL;
  }

  if (ssl_ctx_) {
    SSL_CTX_free(ssl_ctx_);
    ssl_ctx_ = NULL;
  }
}

//
// AsyncSocket Implementation
//

int
OpenSSLAdapter::Send(const void* pv, size_t cb) {
  //LOG(LS_INFO) << "OpenSSLAdapter::Send(" << cb << ")";

  switch (state_) {
  case SSL_NONE:
    return AsyncSocketAdapter::Send(pv, cb);

  case SSL_WAIT:
  case SSL_CONNECTING:
    SetError(EWOULDBLOCK);
    return SOCKET_ERROR;

  case SSL_CONNECTED:
    break;

  case SSL_ERROR:
  default:
    return SOCKET_ERROR;
  }

  // OpenSSL will return an error if we try to write zero bytes
  if (cb == 0)
    return 0;

  ssl_write_needs_read_ = false;

  int code = SSL_write(ssl_, pv, cb);
  switch (SSL_get_error(ssl_, code)) {
  case SSL_ERROR_NONE:
    //LOG(LS_INFO) << " -- success";
    return code;
  case SSL_ERROR_WANT_READ:
    //LOG(LS_INFO) << " -- error want read";
    ssl_write_needs_read_ = true;
    SetError(EWOULDBLOCK);
    break;
  case SSL_ERROR_WANT_WRITE:
    //LOG(LS_INFO) << " -- error want write";
    SetError(EWOULDBLOCK);
    break;
  case SSL_ERROR_ZERO_RETURN:
    //LOG(LS_INFO) << " -- remote side closed";
    SetError(EWOULDBLOCK);
    // do we need to signal closure?
    break;
  default:
    //LOG(LS_INFO) << " -- error " << code;
    Error("SSL_write", (code ? code : -1), false);
    break;
  }

  return SOCKET_ERROR;
}

int
OpenSSLAdapter::Recv(void* pv, size_t cb) {
  //LOG(LS_INFO) << "OpenSSLAdapter::Recv(" << cb << ")";
  switch (state_) {

  case SSL_NONE:
    return AsyncSocketAdapter::Recv(pv, cb);

  case SSL_WAIT:
  case SSL_CONNECTING:
    SetError(EWOULDBLOCK);
    return SOCKET_ERROR;

  case SSL_CONNECTED:
    break;

  case SSL_ERROR:
  default:
    return SOCKET_ERROR;
  }

  // Don't trust OpenSSL with zero byte reads
  if (cb == 0)
    return 0;

  ssl_read_needs_write_ = false;

  int code = SSL_read(ssl_, pv, cb);
  switch (SSL_get_error(ssl_, code)) {
  case SSL_ERROR_NONE:
    //LOG(LS_INFO) << " -- success";
    return code;
  case SSL_ERROR_WANT_READ:
    //LOG(LS_INFO) << " -- error want read";
    SetError(EWOULDBLOCK);
    break;
  case SSL_ERROR_WANT_WRITE:
    //LOG(LS_INFO) << " -- error want write";
    ssl_read_needs_write_ = true;
    SetError(EWOULDBLOCK);
    break;
  case SSL_ERROR_ZERO_RETURN:
    //LOG(LS_INFO) << " -- remote side closed";
    SetError(EWOULDBLOCK);
    // do we need to signal closure?
    break;
  default:
    //LOG(LS_INFO) << " -- error " << code;
    Error("SSL_read", (code ? code : -1), false);
    break;
  }

  return SOCKET_ERROR;
}

int
OpenSSLAdapter::Close() {
  Cleanup();
  state_ = restartable_ ? SSL_WAIT : SSL_NONE;
  return AsyncSocketAdapter::Close();
}

Socket::ConnState
OpenSSLAdapter::GetState() const {
  //if (signal_close_)
  //  return CS_CONNECTED;
  ConnState state = socket_->GetState();
  if ((state == CS_CONNECTED)
      && ((state_ == SSL_WAIT) || (state_ == SSL_CONNECTING)))
    state = CS_CONNECTING;
  return state;
}

void
OpenSSLAdapter::OnConnectEvent(AsyncSocket* socket) {
  LOG(LS_INFO) << "OpenSSLAdapter::OnConnectEvent";
  if (state_ != SSL_WAIT) {
    ASSERT(state_ == SSL_NONE);
    AsyncSocketAdapter::OnConnectEvent(socket);
    return;
  }

  state_ = SSL_CONNECTING;
  if (int err = BeginSSL()) {
    AsyncSocketAdapter::OnCloseEvent(socket, err);
  }
}

void
OpenSSLAdapter::OnReadEvent(AsyncSocket* socket) {
  //LOG(LS_INFO) << "OpenSSLAdapter::OnReadEvent";

  if (state_ == SSL_NONE) {
    AsyncSocketAdapter::OnReadEvent(socket);
    return;
  }

  if (state_ == SSL_CONNECTING) {
    if (int err = ContinueSSL()) {
      Error("ContinueSSL", err);
    }
    return;
  }

  if (state_ != SSL_CONNECTED)
    return;

  // Don't let ourselves go away during the callbacks
  //PRefPtr<OpenSSLAdapter> lock(this); // TODO: fix this
  if (ssl_write_needs_read_)  {
    //LOG(LS_INFO) << " -- onStreamWriteable";
    AsyncSocketAdapter::OnWriteEvent(socket);
  }

  //LOG(LS_INFO) << " -- onStreamReadable";
  AsyncSocketAdapter::OnReadEvent(socket);
}

void
OpenSSLAdapter::OnWriteEvent(AsyncSocket* socket) {
  //LOG(LS_INFO) << "OpenSSLAdapter::OnWriteEvent";

  if (state_ == SSL_NONE) {
    AsyncSocketAdapter::OnWriteEvent(socket);
    return;
  }

  if (state_ == SSL_CONNECTING) {
    if (int err = ContinueSSL()) {
      Error("ContinueSSL", err);
    }
    return;
  }

  if (state_ != SSL_CONNECTED)
    return;

  // Don't let ourselves go away during the callbacks
  //PRefPtr<OpenSSLAdapter> lock(this); // TODO: fix this

  if (ssl_read_needs_write_)  {
    //LOG(LS_INFO) << " -- onStreamReadable";
    AsyncSocketAdapter::OnReadEvent(socket);
  }

  //LOG(LS_INFO) << " -- onStreamWriteable";
  AsyncSocketAdapter::OnWriteEvent(socket);
}

void
OpenSSLAdapter::OnCloseEvent(AsyncSocket* socket, int err) {
  LOG(LS_INFO) << "OpenSSLAdapter::OnCloseEvent(" << err << ")";
  AsyncSocketAdapter::OnCloseEvent(socket, err);
}

// This code is taken from the "Network Security with OpenSSL"
// sample in chapter 5

bool OpenSSLAdapter::VerifyServerName(SSL* ssl, const char* host,
                                      bool ignore_bad_cert) {
  if (!host)
    return false;

  // Checking the return from SSL_get_peer_certificate here is not strictly
  // necessary.  With our setup, it is not possible for it to return
  // NULL.  However, it is good form to check the return.
  X509* certificate = SSL_get_peer_certificate(ssl);
  if (!certificate)
    return false;

#ifdef _DEBUG
  {
    LOG(LS_INFO) << "Certificate from server:";
    BIO* mem = BIO_new(BIO_s_mem());
    X509_print_ex(mem, certificate, XN_FLAG_SEP_CPLUS_SPC, X509_FLAG_NO_HEADER);
    BIO_write(mem, "\0", 1);
    char* buffer;
    BIO_get_mem_data(mem, &buffer);
    LOG(LS_INFO) << buffer;
    BIO_free(mem);

    char* cipher_description =
      SSL_CIPHER_description(SSL_get_current_cipher(ssl), NULL, 128);
    LOG(LS_INFO) << "Cipher: " << cipher_description;
    OPENSSL_free(cipher_description);
  }
#endif

  bool ok = false;
  int extension_count = X509_get_ext_count(certificate);
  for (int i = 0; i < extension_count; ++i) {
    X509_EXTENSION* extension = X509_get_ext(certificate, i);
    int extension_nid = OBJ_obj2nid(X509_EXTENSION_get_object(extension));

    if (extension_nid == NID_subject_alt_name) {
#if OPENSSL_VERSION_NUMBER >= 0x10000000L
      const X509V3_EXT_METHOD* meth = X509V3_EXT_get(extension);
#else
      X509V3_EXT_METHOD* meth = X509V3_EXT_get(extension);
#endif
      if (!meth)
        break;

      void* ext_str = NULL;

      // We assign this to a local variable, instead of passing the address
      // directly to ASN1_item_d2i.
      // See http://readlist.com/lists/openssl.org/openssl-users/0/4761.html.
      unsigned char* ext_value_data = extension->value->data;

#if OPENSSL_VERSION_NUMBER >= 0x0090800fL
      const unsigned char **ext_value_data_ptr =
          (const_cast<const unsigned char **>(&ext_value_data));
#else
      unsigned char **ext_value_data_ptr = &ext_value_data;
#endif

      if (meth->it) {
        ext_str = ASN1_item_d2i(NULL, ext_value_data_ptr,
                                extension->value->length,
                                ASN1_ITEM_ptr(meth->it));
      } else {
        ext_str = meth->d2i(NULL, ext_value_data_ptr, extension->value->length);
      }

      STACK_OF(CONF_VALUE)* value = meth->i2v(meth, ext_str, NULL);
      for (int j = 0; j < sk_CONF_VALUE_num(value); ++j) {
        CONF_VALUE* nval = sk_CONF_VALUE_value(value, j);
        // The value for nval can contain wildcards
        if (!strcmp(nval->name, "DNS") && string_match(host, nval->value)) {
          ok = true;
          break;
        }
      }
      sk_CONF_VALUE_pop_free(value, X509V3_conf_free);
      value = NULL;

      if (meth->it) {
        ASN1_item_free(reinterpret_cast<ASN1_VALUE*>(ext_str), meth->it);
      } else {
        meth->ext_free(ext_str);
      }
      ext_str = NULL;
    }
    if (ok)
      break;
  }

  char data[256];
  X509_name_st* subject;
  if (!ok
      && (subject = X509_get_subject_name(certificate))
      && (X509_NAME_get_text_by_NID(subject, NID_commonName,
                                    data, sizeof(data)) > 0)) {
    data[sizeof(data)-1] = 0;
    if (_stricmp(data, host) == 0)
      ok = true;
  }

  X509_free(certificate);

  // This should only ever be turned on for debugging and development.
  if (!ok && ignore_bad_cert) {
    LOG(LS_WARNING) << "TLS certificate check FAILED.  "
      << "Allowing connection anyway.";
    ok = true;
  }

  return ok;
}

bool OpenSSLAdapter::SSLPostConnectionCheck(SSL* ssl, const char* host) {
  bool ok = VerifyServerName(ssl, host, ignore_bad_cert());

  if (ok) {
    ok = (SSL_get_verify_result(ssl) == X509_V_OK ||
          custom_verification_succeeded_);
  }

  if (!ok && ignore_bad_cert()) {
    LOG(LS_INFO) << "Other TLS post connection checks failed.";
    ok = true;
  }

  return ok;
}

#if _DEBUG

// We only use this for tracing and so it is only needed in debug mode

void
OpenSSLAdapter::SSLInfoCallback(const SSL* s, int where, int ret) {
  const char* str = "undefined";
  int w = where & ~SSL_ST_MASK;
  if (w & SSL_ST_CONNECT) {
    str = "SSL_connect";
  } else if (w & SSL_ST_ACCEPT) {
    str = "SSL_accept";
  }
  if (where & SSL_CB_LOOP) {
    LOG(LS_INFO) <<  str << ":" << SSL_state_string_long(s);
  } else if (where & SSL_CB_ALERT) {
    str = (where & SSL_CB_READ) ? "read" : "write";
    LOG(LS_INFO) <<  "SSL3 alert " << str
      << ":" << SSL_alert_type_string_long(ret)
      << ":" << SSL_alert_desc_string_long(ret);
  } else if (where & SSL_CB_EXIT) {
    if (ret == 0) {
      LOG(LS_INFO) << str << ":failed in " << SSL_state_string_long(s);
    } else if (ret < 0) {
      LOG(LS_INFO) << str << ":error in " << SSL_state_string_long(s);
    }
  }
}

#endif  // _DEBUG

int
OpenSSLAdapter::SSLVerifyCallback(int ok, X509_STORE_CTX* store) {
#if _DEBUG
  if (!ok) {
    char data[256];
    X509* cert = X509_STORE_CTX_get_current_cert(store);
    int depth = X509_STORE_CTX_get_error_depth(store);
    int err = X509_STORE_CTX_get_error(store);

    LOG(LS_INFO) << "Error with certificate at depth: " << depth;
    X509_NAME_oneline(X509_get_issuer_name(cert), data, sizeof(data));
    LOG(LS_INFO) << "  issuer  = " << data;
    X509_NAME_oneline(X509_get_subject_name(cert), data, sizeof(data));
    LOG(LS_INFO) << "  subject = " << data;
    LOG(LS_INFO) << "  err     = " << err
      << ":" << X509_verify_cert_error_string(err);
  }
#endif

  // Get our stream pointer from the store
  SSL* ssl = reinterpret_cast<SSL*>(
                X509_STORE_CTX_get_ex_data(store,
                  SSL_get_ex_data_X509_STORE_CTX_idx()));

  OpenSSLAdapter* stream =
    reinterpret_cast<OpenSSLAdapter*>(SSL_get_app_data(ssl));

  if (!ok && custom_verify_callback_) {
    void* cert =
        reinterpret_cast<void*>(X509_STORE_CTX_get_current_cert(store));
    if (custom_verify_callback_(cert)) {
      stream->custom_verification_succeeded_ = true;
      LOG(LS_INFO) << "validated certificate using custom callback";
      ok = true;
    }
  }

  // Should only be used for debugging and development.
  if (!ok && stream->ignore_bad_cert()) {
    LOG(LS_WARNING) << "Ignoring cert error while verifying cert chain";
    ok = 1;
  }

  return ok;
}

bool OpenSSLAdapter::ConfigureTrustedRootCertificates(SSL_CTX* ctx) {
  // Add the root cert that we care about to the SSL context
#if OPENSSL_VERSION_NUMBER >= 0x0090800fL
   const unsigned char* cert_buffer
#else
   unsigned char* cert_buffer
#endif
    = Equifax_Secure_Certificate_Authority_certificate;
  size_t cert_buffer_len =
      sizeof(Equifax_Secure_Certificate_Authority_certificate);
  X509* cert = d2i_X509(NULL, &cert_buffer, cert_buffer_len);
  if (cert == NULL)
    return false;
  bool success = X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), cert);
  X509_free(cert);
  return success;
}

SSL_CTX*
OpenSSLAdapter::SetupSSLContext() {
  SSL_CTX* ctx = SSL_CTX_new(TLSv1_client_method());
  if (ctx == NULL)
    return NULL;

  if (!ConfigureTrustedRootCertificates(ctx)) {
    SSL_CTX_free(ctx);
    return NULL;
  }

#ifdef _DEBUG
  SSL_CTX_set_info_callback(ctx, SSLInfoCallback);
#endif

  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, SSLVerifyCallback);
  SSL_CTX_set_verify_depth(ctx, 4);
  SSL_CTX_set_cipher_list(ctx, "ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");

  return ctx;
}

} // namespace talk_base

#endif  // HAVE_OPENSSL_SSL_H
