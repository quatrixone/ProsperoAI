// SPDX-License-Identifier: GPL-3.0-or-later
#define main unused_handler_main
#include "http_api_harness.cpp"
#undef main
#include <cassert>
#include <condition_variable>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>

namespace prospero_model_download
{
bool download_voice_assets()
{
    return true;
}
} // namespace prospero_model_download

extern "C"
{
    int scePthreadCreate(void **out, const void *attrs, void *(*entry)(void *), void *arg,
                         const char *)
    {
        pthread_t thread{};
        int rc = pthread_create(&thread, static_cast<const pthread_attr_t *>(attrs), entry, arg);
        if (!rc)
            *out = reinterpret_cast<void *>(thread);
        return rc;
    }
    int scePthreadJoin(void *thread, void **result)
    {
        return pthread_join(reinterpret_cast<pthread_t>(thread), result);
    }
    int sceNetInit()
    {
        return 0;
    }
    int sceNetPoolCreate(const char *, int, int)
    {
        return 1;
    }
    int sceNetSocket(const char *, int, int, int)
    {
        return socket(AF_INET, SOCK_STREAM, 0);
    }
    int sceNetSocketClose(int fd)
    {
        return close(fd);
    }
    int sceNetConnect(int, const void *, unsigned int)
    {
        return -1;
    }
    int sceNetBind(int fd, const void *raw, unsigned int)
    {
        const auto *ps5 = static_cast<const SceNetSockaddrIn *>(raw);
        sockaddr_in host{};
        host.sin_family = AF_INET;
        host.sin_port = ps5->sin_port;
        host.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return bind(fd, reinterpret_cast<sockaddr *>(&host), sizeof(host));
    }
    int sceNetListen(int fd, int backlog)
    {
        return listen(fd, backlog);
    }
    int sceNetAccept(int fd, void *, unsigned int *)
    {
        return accept(fd, nullptr, nullptr);
    }
    int sceNetSend(int fd, const void *bytes, unsigned long size, int)
    {
        return send(fd, bytes, size, MSG_NOSIGNAL);
    }
    int sceNetRecv(int fd, void *bytes, unsigned long size, int flags)
    {
        return recv(fd, bytes, size, flags);
    }
    int sceNetSetsockopt(int fd, int, int option, const void *value, unsigned int size)
    {
        int host = option == kSoReuseAddr        ? SO_REUSEADDR
                   : option == kSoReceiveTimeout ? SO_RCVTIMEO
                                                 : SO_SNDTIMEO;
        return setsockopt(fd, SOL_SOCKET, host, value, size);
    }
}

int connect_to(unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
    {
        close(fd);
        return -1;
    }
    timeval timeout{8, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}
std::string request(unsigned short port, const char *body)
{
    int fd = connect_to(port);
    assert(fd >= 0);
    std::string req = "POST /api/generate HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
                      std::to_string(std::strlen(body)) + "\r\n\r\n" + body;
    assert(send(fd, req.data(), req.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(req.size()));
    std::string response;
    char data[4096];
    ssize_t n;
    while ((n = recv(fd, data, sizeof(data), 0)) > 0)
        response.append(data, n);
    close(fd);
    return response;
}
int main()
{
    // Reserve an ephemeral port, then let the real server's adapter bind it.
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    assert(probe >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(probe, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(probe, reinterpret_cast<sockaddr *>(&addr), &len) == 0);
    const auto port = ntohs(addr.sin_port);
    close(probe);
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    generation_hook = [&]
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    };
    assert(prospero_http_server_start(port));
    int idle = -1;
    for (unsigned i = 0; i < 200 && idle < 0; ++i)
    {
        idle = connect_to(port);
        if (idle < 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(idle >= 0); // incomplete peer must not hold shutdown forever
    constexpr auto body = R"({"async": true, "model":"first.gguf","prompt":"hello"})";
    const auto accepted = request(port, body);
    assert(accepted.find("200 OK") != std::string::npos);
    assert(accepted.find("job_id") != std::string::npos);
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }));
    }
    assert(request(port, body).find("409 Conflict") != std::string::npos);
    std::atomic<bool> stopped{false};
    std::thread stopping(
        [&]
        {
            prospero_http_server_stop();
            stopped.store(true);
        });
    while (!server_stopping.load())
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(!stopped.load()); // active generation still owns its resources
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    stopping.join();
    close(idle);
    assert(stopped.load() && server_thread == nullptr);
    assert(jobs.size() == 1 && jobs[0]->thread == nullptr && jobs[0]->state == 1);
    assert(connect_to(port) == -1);
    assert(!prospero_http_server_start(port)); // no admission after shutdown
    generation_hook = {};
}
