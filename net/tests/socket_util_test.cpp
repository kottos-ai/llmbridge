// Tests for net/socket_util.hpp over real loopback sockets. Covers the flag
// setters (verified via getsockopt/fcntl), the non-blocking listener, and the
// non-blocking connect path (success, connection-refused, invalid address).

#include "net/socket_util.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace kottos::net;

namespace
{
    int tcp_socket() { return ::socket(AF_INET, SOCK_STREAM, 0); }

    uint16_t port_of(int fd)
    {
        sockaddr_in a{};
        socklen_t len = sizeof(a);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) < 0) return 0;
        return ntohs(a.sin_port);
    }

    bool getsockflag(int fd, int level, int opt)
    {
        int v = 0;
        socklen_t len = sizeof(v);
        ::getsockopt(fd, level, opt, &v, &len);
        return v != 0;
    }

    // Wait up to timeout_ms for the fd to become writable (connect complete).
    bool wait_writable(int fd, int timeout_ms)
    {
        pollfd p{fd, POLLOUT, 0};
        return ::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLOUT);
    }

    bool wait_readable(int fd, int timeout_ms)
    {
        pollfd p{fd, POLLIN, 0};
        return ::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN);
    }
} // namespace

// ── flag setters ────────────────────────────────────────────────────────────
TEST(SocketUtil, SetNonblockingSetsOFlag)
{
    int fd = tcp_socket();
    ASSERT_GE(fd, 0);
    EXPECT_FALSE(::fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    EXPECT_TRUE(set_nonblocking(fd));
    EXPECT_TRUE(::fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    ::close(fd);
}

TEST(SocketUtil, SetNonblockingOnBadFdReturnsFalse)
{
    EXPECT_FALSE(set_nonblocking(-1));
}

TEST(SocketUtil, SetNonblockingIsIdempotent)
{
    int fd = tcp_socket();
    ASSERT_GE(fd, 0);
    EXPECT_TRUE(set_nonblocking(fd));
    EXPECT_TRUE(set_nonblocking(fd));
    EXPECT_TRUE(::fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    ::close(fd);
}

TEST(SocketUtil, SetNodelaySetsTcpNodelay)
{
    int fd = tcp_socket();
    ASSERT_GE(fd, 0);
    set_nodelay(fd);
    EXPECT_TRUE(getsockflag(fd, IPPROTO_TCP, TCP_NODELAY));
    ::close(fd);
}

TEST(SocketUtil, SetNosigpipeSetsSoNosigpipe)
{
    int fd = tcp_socket();
    ASSERT_GE(fd, 0);
    // set_nosigpipe must never crash and must leave the fd usable. On platforms
    // with SO_NOSIGPIPE (macOS/BSD) it sets the per-socket flag; on Linux there
    // is no such option (SIGPIPE is ignored process-wide), so it's a no-op and
    // there is nothing to read back.
    set_nosigpipe(fd);
#ifdef SO_NOSIGPIPE
    EXPECT_TRUE(getsockflag(fd, SOL_SOCKET, SO_NOSIGPIPE));
#endif
    ::close(fd);
}

TEST(SocketUtil, FlagSettersOnBadFdDoNotCrash)
{
    set_nodelay(-1);
    set_nosigpipe(-1);
    SUCCEED();
}

// ── make_listener ─────────────────────────────────────────────────────────
TEST(SocketUtil, MakeListenerReturnsValidFd)
{
    int fd = make_listener(0); // ephemeral
    ASSERT_GE(fd, 0);
    ::close(fd);
}

TEST(SocketUtil, MakeListenerIsNonblocking)
{
    int fd = make_listener(0);
    ASSERT_GE(fd, 0);
    EXPECT_TRUE(::fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    ::close(fd);
}

TEST(SocketUtil, MakeListenerSetsReuseAddrAndPort)
{
    int fd = make_listener(0);
    ASSERT_GE(fd, 0);
    EXPECT_TRUE(getsockflag(fd, SOL_SOCKET, SO_REUSEADDR));
    EXPECT_TRUE(getsockflag(fd, SOL_SOCKET, SO_REUSEPORT));
    ::close(fd);
}

TEST(SocketUtil, MakeListenerEphemeralGetsRealPort)
{
    int fd = make_listener(0);
    ASSERT_GE(fd, 0);
    EXPECT_GT(port_of(fd), 0);
    ::close(fd);
}

TEST(SocketUtil, MakeListenerAcceptIsNonblockingEagainWhenIdle)
{
    int fd = make_listener(0);
    ASSERT_GE(fd, 0);
    int c = ::accept(fd, nullptr, nullptr);
    EXPECT_LT(c, 0);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    ::close(fd);
}

TEST(SocketUtil, MakeListenerReusePortAllowsSecondBind)
{
    int a = make_listener(0);
    ASSERT_GE(a, 0);
    uint16_t p = port_of(a);
    ASSERT_GT(p, 0);
    int b = make_listener(p); // SO_REUSEPORT -> second bind on same port succeeds
    EXPECT_GE(b, 0);
    if (b >= 0) ::close(b);
    ::close(a);
}

// ── start_connect / connect_result ──────────────────────────────────────────
TEST(SocketUtil, StartConnectToListenerSucceeds)
{
    int lst = make_listener(0);
    ASSERT_GE(lst, 0);
    uint16_t p = port_of(lst);

    int c = start_connect("127.0.0.1", p);
    ASSERT_GE(c, 0);
    ASSERT_TRUE(wait_writable(c, 1000));
    EXPECT_EQ(connect_result(c), 0);

    ASSERT_TRUE(wait_readable(lst, 1000)); // connection landed in the accept queue
    int s = ::accept(lst, nullptr, nullptr);
    EXPECT_GE(s, 0);
    if (s >= 0) ::close(s);
    ::close(c);
    ::close(lst);
}

TEST(SocketUtil, ConnectedSocketIsNonblocking)
{
    int lst = make_listener(0);
    ASSERT_GE(lst, 0);
    int c = start_connect("127.0.0.1", port_of(lst));
    ASSERT_GE(c, 0);
    EXPECT_TRUE(::fcntl(c, F_GETFL, 0) & O_NONBLOCK);
    ::close(c);
    ::close(lst);
}

TEST(SocketUtil, StartConnectToRefusedPortReportsError)
{
    // Bind+immediately close a listener to obtain a (very likely) free port.
    int tmp = make_listener(0);
    ASSERT_GE(tmp, 0);
    uint16_t p = port_of(tmp);
    ::close(tmp); // nothing listening on p now

    int c = start_connect("127.0.0.1", p);
    ASSERT_GE(c, 0);
    wait_writable(c, 1000); // becomes "writable" with a pending SO_ERROR
    EXPECT_NE(connect_result(c), 0); // ECONNREFUSED (or similar)
    ::close(c);
}

// Invalid addresses -> inet_pton fails -> start_connect returns -1.
class StartConnectBadIp : public ::testing::TestWithParam<const char*> {};
TEST_P(StartConnectBadIp, ReturnsMinusOne)
{
    EXPECT_EQ(start_connect(GetParam(), 8080), -1);
}
INSTANTIATE_TEST_SUITE_P(Cases, StartConnectBadIp,
                         ::testing::Values("999.999.999.999", "not.an.ip", "256.0.0.1",
                                           "1.2.3", "", "localhost", "12.34.56.78.90",
                                           "::1", "1.2.3.4.5"),
                         [](const testing::TestParamInfo<const char*>& i) {
                             std::string s(i.param);
                             for (char& ch : s) if (!std::isalnum((unsigned char)ch)) ch = '_';
                             return s.empty() ? std::string("empty") : s;
                         });
