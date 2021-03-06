#include "server.h"
//#include "html.h"

#if HAVE_OPENSSL
#include <openssl/ssl.h>
#endif

#if ! COMPILED_IN_RESOURCES && ! defined(LWS_WITH_ZIP_FOPS)
#error Must configure --enable-compiled-in-resources since zip support missing in libwebsockets
#endif

#if LWS_LIBRARY_VERSION_NUMBER < (3*1000000+0*1000+0)
// Copied and simplified from libwebsockets 3.x source.
#ifndef LWS_ILLEGAL_HTTP_CONTENT_LEN
#define LWS_ILLEGAL_HTTP_CONTENT_LEN ((lws_filepos_t)-1ll)
#endif
static int
lws_add_http_common_headers(struct lws *wsi, unsigned int code,
                            const char *content_type, lws_filepos_t content_len,
                            unsigned char **p, unsigned char *end)
{
    if (lws_add_http_header_status(wsi, code, p, end))
        return 1;
    if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                                     (unsigned char *)content_type,
                                     (int)strlen(content_type), p, end))
        return 1;
    if (content_len != LWS_ILLEGAL_HTTP_CONTENT_LEN
        && lws_add_http_header_content_length(wsi, content_len,
                                              p, end))
        return 1;
    return 0;
}
static int
lws_finalize_write_http_header(struct lws *wsi, unsigned char *start,
                               unsigned char **pp, unsigned char *end)
{
    unsigned char *p;
    int len;
    if (lws_finalize_http_header(wsi, pp, end))
        return 1;
    p = *pp;
    len = (int)((char *)p - (char *)start);
    if (lws_write(wsi, start, len, LWS_WRITE_HTTP_HEADERS) != len)
        return 1;
    return 0;
}
#endif

#ifdef RESOURCE_DIR
static char *resource_path = NULL;
static char domterm_jar_name[] = "domterm.jar";
char *
get_resource_path()
{
  if (resource_path == NULL) {
      resource_path = RESOURCE_DIR;
      if (resource_path[0] != '/') {
          char *cmd_path = get_executable_path();
          int cmd_dir_length = get_executable_directory_length();
          char *buf = xmalloc(cmd_dir_length + strlen(resource_path)
                             + sizeof(domterm_jar_name) + 2);
          sprintf(buf, "%.*s/%s/%s", cmd_dir_length, cmd_path, resource_path,
                  domterm_jar_name);
          resource_path = buf;
      }
  }
  return resource_path;
}
#endif

const char * get_mimetype(const char *file)
{
        int n = strlen(file);

        if (n < 5)
                return NULL;

        if (!strcmp(&file[n - 4], ".ico"))
                return "image/x-icon";

        if (!strcmp(&file[n - 4], ".png"))
                return "image/png";

        if (!strcmp(&file[n - 4], ".svg"))
                return "image/svg+xml";

        if (!strcmp(&file[n - 5], ".jpeg")
            ||!strcmp(&file[n - 4], ".jpg"))
                return "text/jpeg";

        if (!strcmp(&file[n - 5], ".html"))
                return "text/html";

        if (!strcmp(&file[n - 4], ".css"))
                return "text/css";

        if (!strcmp(&file[n - 3], ".js"))
                return "text/javascript";

        return NULL;
}

int
check_auth(struct lws *wsi) {
    if (server->options.credential == NULL)
        return 0;

    int hdr_length = lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_AUTHORIZATION);
    char buf[hdr_length + 1];
    int len = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_HTTP_AUTHORIZATION);
    if (len > 0) {
        // extract base64 text from authorization header
        char *ptr = &buf[0];
        char *token, *b64_text = NULL;
        int i = 1;
        while ((token = strsep(&ptr, " ")) != NULL) {
            if (strlen(token) == 0)
                continue;
            if (i++ == 2) {
                b64_text = token;
                break;
            }
        }
        if (b64_text != NULL && !strcmp(b64_text, server->options.credential))
            return 0;
    }

    unsigned char buffer[1024 + LWS_PRE], *p, *end;
    p = buffer + LWS_PRE;
    end = p + sizeof(buffer) - LWS_PRE;

    if (lws_add_http_header_status(wsi, HTTP_STATUS_UNAUTHORIZED, &p, end))
        return 1;
    if (lws_add_http_header_by_token(wsi,
                                     WSI_TOKEN_HTTP_WWW_AUTHENTICATE,
                                     (unsigned char *) "Basic realm=\"ttyd\"",
                                     18, &p, end))
        return 1;
    if (lws_add_http_header_content_length(wsi, 0, &p, end))
        return 1;
    if (lws_finalize_http_header(wsi, &p, end))
        return 1;
    if (lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE), LWS_WRITE_HTTP_HEADERS) < 0)
        return 1;

    return -1;
}

bool check_server_key(struct lws *wsi, char *arg, size_t alen)
{
    const char*server_key_arg = lws_get_urlarg_by_name(wsi, "server-key=", arg, alen);
    if (server_key_arg != NULL &&
        memcmp(server_key_arg, server_key, SERVER_KEY_LENGTH) == 0)
      return true;
    if (main_options->http_server)
        return true;
    lwsl_notice("missing or non-matching server-key!\n");
    lws_return_http_status(wsi, HTTP_STATUS_UNAUTHORIZED, NULL);
    return false;
}

#define LBUFSIZE 4096
int
write_simple_response(struct lws *wsi, struct http_client *hclient,
                      const char *content_type,
                      unsigned char *content_data, unsigned int content_length,
                      bool owns_data, unsigned char *buffer)
{
    uint8_t *start = buffer+LWS_PRE, *p = start,
        *end = &buffer[LBUFSIZE - LWS_PRE - 1];

    if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK,
                                    content_type, content_length,
                                    &p, end))
        return 1;
    if (lws_finalize_write_http_header(wsi, start, &p, end))
        return 1;

    hclient->owns_data = owns_data;
    hclient->data = content_data;
    hclient->ptr = content_data;
    hclient->length = content_length;
    /* write the body separately */
    lws_callback_on_writable(wsi);
    return 0;
}

/** Callack for servering http - generally static files. */

int
callback_http(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    struct http_client *hclient = (struct http_client *) user;
    unsigned char buffer[LBUFSIZE + LWS_PRE], *start = &buffer[LWS_PRE], *p, *end;
    char buf[256];

    switch (reason) {
        case LWS_CALLBACK_HTTP:
            {
                char name[100], rip[50];
                lws_get_peer_addresses(wsi, lws_get_socket_fd(wsi), name, sizeof(name), rip, sizeof(rip));
                lwsl_notice("HTTP connect from %s (%s), path: %s\n",
                            name, rip, (char *) in);
            }

            if (len < 1) {
                lws_return_http_status(wsi, HTTP_STATUS_BAD_REQUEST, NULL);
                goto try_to_reuse;
            }

            switch (check_auth(wsi)) {
                case 0:
                    break;
                case -1:
                    goto try_to_reuse;
                case 1:
                default:
                    return 1;
            }

            // if a legal POST URL, let it continue and accept data
            if (lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI))
                return 0;

            p = buffer + LWS_PRE;
            end = p + sizeof(buffer) - LWS_PRE;
#if 0
            if (server->client_can_close
                && strncmp((const char *) in, "/(WINDOW-CLOSED)", 16) == 0) {
                force_exit = true;
                lws_cancel_service(context);
                exit(0);
            }
#endif
            if (!strncmp((const char *) in, "/auth_token.js", 14)) {
                size_t n = server->options.credential != NULL ? sprintf(buf, "var tty_auth_token = '%s';", server->options.credential) : 0;

                if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK,
                                            "application/javascript",
                                                (unsigned long) n, &p, end)
                    || lws_finalize_write_http_header(wsi, buffer+LWS_PRE,
                                                      &p, end))
                    return 1;
                if (n > 0 && lws_write_http(wsi, buf, n) < 0) {
                    return 1;
                }
                goto try_to_reuse;
            }

            const char saved_prefix[] = "/saved-file/";
            size_t saved_prefix_len = sizeof(saved_prefix)-1;
            if (!strncmp((const char *) in, saved_prefix, saved_prefix_len)) {
                int blen = lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_URI_ARGS);
                char *buf = xmalloc(blen+1);
                const char *filename = NULL;
                FILE *sfile = NULL;
                struct stat stbuf;
                off_t slen;
                int fd = -1;
                int ret = -1;
                if (check_server_key(wsi, buf, blen)
                    && (filename = lws_get_urlarg_by_name(wsi, "file=", buf, blen)) != NULL
                    && (fd = open(filename, O_RDONLY)) >= 0
                    && fstat(fd, &stbuf) == 0
                    && (slen = stbuf.st_size) > 0
                    && (buf = realloc(buf, slen)) != NULL
                    && (sfile = fdopen(fd, "r")) != NULL
                    && fread(buf, 1, slen, sfile) == slen) {
                    struct sbuf sb[1];
                    sbuf_init(sb);
                    // FIXME: We should encrypt the response (perhaps just a
                    // simple encryption using the kerver_key).  It is probably
                    // not an issue for local requests, and for non-local
                    // requests (where one should use tls or ssh).
                    make_html_text(sb, http_port, true, buf, slen);
                    char *data = sb->buffer;
                    int dlen = sb->len;
                    sb->buffer = NULL;
                    sbuf_free(sb);
                    ret = write_simple_response(wsi, hclient, "text/html",
                                                data, dlen,
                                                true, buffer);
                    buf = NULL; // buf is now owned by write_simple_response
                }
                if (buf != NULL)
                    free(buf);
                if (sfile != NULL)
                    fclose(sfile);
                else if (fd >= 0)
                    close(fd);
                return ret;
            }
            const char* fname = in;
            if (fname == NULL || strcmp(fname, "/") == 0) {
                if (main_options->http_server)
                    fname = main_html_path;
                else
                    fname = "/repl-client.html";
            }
            const char* content_type = get_mimetype(fname);
            if (content_type == NULL)
              content_type = "text/html";
            if (strcmp(fname, "/simple.html") == 0) {
                struct sbuf sb[1];
                sbuf_init(sb);
                make_html_text(sb, http_port, true, NULL, 0);
                char *data = sb->buffer;
                int dlen = sb->len;
                sb->buffer = NULL;
                sbuf_free(sb);
                return write_simple_response(wsi, hclient, content_type,
                                             data, dlen,
                                             true, buffer);
            }
#if COMPILED_IN_RESOURCES
            struct resource *resource = &resources[0];
            while (resource->name != NULL && strcmp(resource->name, fname+1) != 0)
                resource++;
            if (resource->name != NULL) {
                return write_simple_response(wsi, hclient, content_type,
                                             resource->data, resource->length,
                                             false, buffer);
            }
            lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
            goto try_to_reuse;
#else
            const char* buf = fname;
            int n = lws_serve_http_file(wsi, buf, content_type, NULL, 0);
            if (n < 0 || ((n > 0) && lws_http_transaction_completed(wsi)))
              return -1; /* error or can't reuse connection: close the socket */
            break;
#endif

        case LWS_CALLBACK_HTTP_WRITEABLE:
            if (hclient->length) {
                int max_chunk = 2000;
                int cur_chunk = hclient->length > max_chunk ? max_chunk : hclient->length;
                hclient->length -= cur_chunk;
                if (lws_write(wsi, (uint8_t *)hclient->ptr, cur_chunk,
                              hclient->length > 0 ? LWS_WRITE_HTTP : LWS_WRITE_HTTP_FINAL)
                    != cur_chunk)
                    return 1;
                if (hclient->length > 0) {
                    hclient->ptr += cur_chunk;
                    lws_callback_on_writable(wsi);
                } else {
                    if (hclient->owns_data)
                        free(hclient->data);
                    hclient->data = NULL;
                    hclient->ptr = NULL;
                    if (lws_http_transaction_completed(wsi))
                        return -1;
                }
                return 0;
            }
            break;

	case LWS_CALLBACK_HTTP_FILE_COMPLETION:
            if (lws_http_transaction_completed(wsi))
              return -1; /* error or can't reuse connection: close the socket */
            break;

#if HAVE_OPENSSL
        case LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION:
            if (!len || (SSL_get_verify_result((SSL *) in) != X509_V_OK)) {
                int err = X509_STORE_CTX_get_error((X509_STORE_CTX *) user);
                int depth = X509_STORE_CTX_get_error_depth((X509_STORE_CTX *) user);
                const char *msg = X509_verify_cert_error_string(err);
                lwsl_err("client certificate verification error: %s (%d), depth: %d\n", msg, err, depth);
                return 1;
            }
            break;
#endif

        default:
            return lws_callback_http_dummy(wsi, reason, user, in, len);
    }

    return 0;

    /* if we're on HTTP1.1 or 2.0, will keep the idle connection alive */
    try_to_reuse:
    if (lws_http_transaction_completed(wsi))
        return -1;

    return 0;
}
