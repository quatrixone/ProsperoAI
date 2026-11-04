/*
 * ProsperoAI - HTTPS downloads, read a piece at a time.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "https_get.h"

#include "console_curl.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* What may wait between libcurl and the reader. A chunk that does not fit pauses the
 * transfer until the reader has caught up, so a slow disk holds the network back
 * instead of filling memory. */
#define HTTPS_BUFFER_BYTES (4u * 1024u * 1024u)

struct prospero_https
{
    CURLM *multi;
    CURL *easy;
    char *data;
    size_t capacity;
    size_t have, taken;
    int done, paused;
    CURLcode result;
    prospero_https_stop stop;
    void *stop_user;
    struct curl_slist *headers;      /* set by the owner, kept for the session */
    struct curl_slist *request_headers; /* headers owned by the request in flight */
    char *user_agent;
    int cookies, compression;
    char error[CURL_ERROR_SIZE];
};

#define HTTPS_BUFFER_MINIMUM (16u * 1024u)
#define HTTPS_BUFFER_MAXIMUM (8u * 1024u * 1024u)

static pthread_once_t global_once = PTHREAD_ONCE_INIT;
static CURLcode global_result;
static int receive_buffer = -1; /* what the last socket was given */
static pthread_mutex_t receive_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

static void global_start(void)
{
    global_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}

/* The console gives a socket a 64 KB receive buffer. A sender may have that much on
 * its way and no more, so a server 60 ms away delivers about 1 MB/s whatever the line
 * could carry (measured in ProsperoTV: 1.07 MB/s where a PC on the same network got
 * 18 MB/s). The buffer is asked for before the connection is made, when the window's
 * scale is agreed: the largest size the console accepts. */
static int on_socket(void *user, curl_socket_t socket, curlsocktype purpose)
{
    (void)user;
    (void)purpose;
    (void)console_curl_nonblocking(socket); /* as console_curl_setup() does */
    int granted = 0;
    for (int bytes = 8 * 1024 * 1024; bytes >= 256 * 1024; bytes /= 2)
        if (setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0)
        {
            granted = bytes;
            break;
        }
    pthread_mutex_lock(&receive_buffer_mutex);
    receive_buffer = granted;
    pthread_mutex_unlock(&receive_buffer_mutex);
    return CURL_SOCKOPT_OK;
}

int prospero_https_receive_buffer(void)
{
    pthread_mutex_lock(&receive_buffer_mutex);
    const int result = receive_buffer;
    pthread_mutex_unlock(&receive_buffer_mutex);
    return result;
}

static size_t on_data(char *bytes, size_t size, size_t count, void *user)
{
    prospero_https *session = user;
    const size_t length = size * count;
    if (session->taken == session->have)
        session->have = session->taken = 0;
    if (session->have + length > session->capacity)
    {
        if (session->taken != 0)
        {
            memmove(session->data, session->data + session->taken, session->have - session->taken);
            session->have -= session->taken;
            session->taken = 0;
        }
        if (session->have + length > session->capacity)
        {
            session->paused = 1;
            return CURL_WRITEFUNC_PAUSE;
        }
    }
    memcpy(session->data + session->have, bytes, length);
    session->have += length;
    return length;
}

/* True once the owner wants the request to end; the request then counts as failed. */
static int stopped(prospero_https *session)
{
    if (!session->stop || !session->stop(session->stop_user))
        return 0;
    session->done = 1;
    session->result = CURLE_ABORTED_BY_CALLBACK;
    snprintf(session->error, sizeof(session->error), "stopped");
    return 1;
}

static void drop_request(prospero_https *session)
{
    if (!session->easy)
        return;
    curl_multi_remove_handle(session->multi, session->easy);
    curl_easy_cleanup(session->easy);
    session->easy = NULL;
}

/* One step of the transfer: what is ready is moved, and the result is kept when it ends. */
static void pump(prospero_https *session)
{
    int running = 0;
    if (session->paused && session->have == session->taken)
    {
        session->have = session->taken = 0;
        session->paused = 0;
        curl_easy_pause(session->easy, CURLPAUSE_CONT);
    }
    CURLMcode code = curl_multi_perform(session->multi, &running);
    if (code == CURLM_OK && running && session->have == session->taken && !session->paused)
        code = curl_multi_poll(session->multi, NULL, 0, 250, NULL);
    if (code != CURLM_OK)
    {
        snprintf(session->error, sizeof(session->error), "%s", curl_multi_strerror(code));
        session->result = CURLE_RECV_ERROR;
        session->done = 1;
        return;
    }
    int left = 0;
    for (CURLMsg *message; (message = curl_multi_info_read(session->multi, &left)) != NULL;)
        if (message->msg == CURLMSG_DONE && message->easy_handle == session->easy)
        {
            session->result = message->data.result;
            session->done = 1;
            if (session->result != CURLE_OK && !session->error[0])
                snprintf(session->error, sizeof(session->error), "%s",
                         curl_easy_strerror(session->result));
        }
}

prospero_https *prospero_https_open(void)
{
    return prospero_https_open_sized(HTTPS_BUFFER_BYTES);
}

prospero_https *prospero_https_open_sized(size_t bytes)
{
    pthread_once(&global_once, global_start);
    if (global_result != CURLE_OK)
        return NULL;
    if (bytes < HTTPS_BUFFER_MINIMUM)
        bytes = HTTPS_BUFFER_MINIMUM;
    if (bytes > HTTPS_BUFFER_MAXIMUM)
        bytes = HTTPS_BUFFER_MAXIMUM;
    prospero_https *session = calloc(1, sizeof(*session));
    if (!session)
        return NULL;
    session->multi = curl_multi_init();
    session->capacity = bytes;
    session->data = malloc(bytes);
    if (!session->multi || !session->data)
    {
        prospero_https_close(session);
        return NULL;
    }
    return session;
}

int prospero_https_add_header(prospero_https *session, const char *line)
{
    if (!session || !line || !*line)
        return 1;
    struct curl_slist *grown = curl_slist_append(session->headers, line);
    if (!grown)
        return 1;
    session->headers = grown;
    return 0;
}

void prospero_https_clear_headers(prospero_https *session)
{
    if (!session || !session->headers)
        return;
    curl_slist_free_all(session->headers);
    session->headers = NULL;
}

void prospero_https_set_user_agent(prospero_https *session, const char *user_agent)
{
    if (!session)
        return;
    free(session->user_agent);
    session->user_agent = NULL;
    if (user_agent && *user_agent)
    {
        const size_t length = strlen(user_agent) + 1;
        session->user_agent = malloc(length);
        if (session->user_agent)
            memcpy(session->user_agent, user_agent, length);
    }
}

void prospero_https_enable_cookies(prospero_https *session)
{
    if (session)
        session->cookies = 1;
}

void prospero_https_set_compression(prospero_https *session, int enabled)
{
    if (session)
        session->compression = enabled ? 1 : 0;
}

void prospero_https_set_stop(prospero_https *session, prospero_https_stop stop, void *user)
{
    session->stop = stop;
    session->stop_user = user;
}

void prospero_https_close(prospero_https *session)
{
    if (!session)
        return;
    drop_request(session);
    if (session->multi)
        curl_multi_cleanup(session->multi);
    prospero_https_clear_headers(session);
    if (session->request_headers)
        curl_slist_free_all(session->request_headers);
    free(session->user_agent);
    free(session->data);
    free(session);
}

int prospero_https_get(prospero_https *session, const char *url, long *status)
{
    return prospero_https_get_from(session, url, 0, status);
}

static int get_request(prospero_https *session, const char *url, const char *range,
                       const char *body, const char *content_type, long *status);

int prospero_https_post(prospero_https *session, const char *url, const char *body,
                        const char *content_type, long *status)
{
    if (!session || !url || !body)
        return 1;
    return get_request(session, url, NULL, body,
                       content_type ? content_type : "Content-Type: application/json", status);
}

static int get_request(prospero_https *session, const char *url, const char *range,
                       const char *body, const char *content_type, long *status)
{
    drop_request(session);
    session->have = session->taken = 0;
    session->done = session->paused = 0;
    session->result = CURLE_OK;
    session->error[0] = '\0';
    session->easy = curl_easy_init();
    if (!session->easy)
    {
        snprintf(session->error, sizeof(session->error), "libcurl could not make a request");
        return 1;
    }
    CURL *easy = session->easy;
    console_curl_setup(easy);
    curl_easy_setopt(easy, CURLOPT_SOCKOPTFUNCTION, on_socket);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_USERAGENT,
                     session->user_agent ? session->user_agent : "ProsperoAI/1.0");
    if (session->headers)
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, session->headers);
    if (session->cookies)
        curl_easy_setopt(easy, CURLOPT_COOKIEFILE, ""); /* in memory, for this session only */
    if (session->compression)
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    if (body)
    {
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
        if (content_type)
        {
            /* The body's type belongs to this request, not to the session. */
            struct curl_slist *typed = curl_slist_append(NULL, content_type);
            if (typed)
            {
                for (struct curl_slist *at = session->headers; at; at = at->next)
                {
                    struct curl_slist *grown = curl_slist_append(typed, at->data);
                    if (!grown)
                        break;
                    typed = grown;
                }
                curl_slist_free_all(session->request_headers);
                session->request_headers = typed;
                curl_easy_setopt(easy, CURLOPT_HTTPHEADER, typed);
            }
        }
    }
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);
    /* Fail a stalled range promptly so its saved offset can be retried. */
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 32768L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, 512L * 1024L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, session);
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, session->error);
    if (range)
        curl_easy_setopt(easy, CURLOPT_RANGE, range);
    if (curl_multi_add_handle(session->multi, easy) != CURLM_OK)
    {
        snprintf(session->error, sizeof(session->error), "libcurl could not start the request");
        return 1;
    }
    while (!session->done && session->have == session->taken && !session->paused)
    {
        if (stopped(session))
            return 1;
        pump(session);
    }
    if (session->done && session->result != CURLE_OK)
        return 1;
    long code = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
    if (status)
        *status = code;
    return 0;
}

int prospero_https_get_from(prospero_https *session, const char *url, unsigned long long offset,
                            long *status)
{
    char range[32];
    if (!offset)
        return get_request(session, url, NULL, NULL, NULL, status);
    snprintf(range, sizeof(range), "%llu-", offset);
    return get_request(session, url, range, NULL, NULL, status);
}

int prospero_https_get_range(prospero_https *session, const char *url,
                             unsigned long long first, unsigned long long last, long *status)
{
    if (last < first)
        return 1;
    char range[64];
    snprintf(range, sizeof(range), "%llu-%llu", first, last);
    return get_request(session, url, range, NULL, NULL, status);
}

int prospero_https_read(prospero_https *session, void *buffer, size_t size)
{
    if (!session->easy || stopped(session))
        return -1;
    while (session->have == session->taken && !session->done)
    {
        pump(session);
        if (stopped(session))
            return -1;
    }
    size_t ready = session->have - session->taken;
    if (ready == 0)
        return session->result == CURLE_OK ? 0 : -1;
    if (ready > size)
        ready = size;
    if (ready > 0x40000000u)
        ready = 0x40000000u;
    memcpy(buffer, session->data + session->taken, ready);
    session->taken += ready;
    return (int)ready;
}

const char *prospero_https_error(const prospero_https *session)
{
    return session && session->error[0] ? session->error : "no answer";
}
