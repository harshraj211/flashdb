/**
 * @file Platform.h
 * @brief Cross-platform socket abstraction layer.
 *
 * Wraps the differences between POSIX sockets (Linux/macOS) and Winsock2
 * (Windows) behind a unified API so FlashDB compiles and runs on both.
 *
 * Key differences handled:
 *   POSIX                       Windows (Winsock2)
 *   ─────────────────────────   ─────────────────────────
 *   #include <sys/socket.h>     #include <winsock2.h>
 *   int fd                      SOCKET fd (UINT_PTR)
 *   close(fd)                   closesocket(fd)
 *   read(fd, buf, len)          recv(fd, buf, len, 0)
 *   write(fd, buf, len)         send(fd, buf, len, 0)
 *   errno                       WSAGetLastError()
 *   SIGPIPE                     (not applicable)
 *   No init needed              WSAStartup() / WSACleanup()
 */

#pragma once

// ============================================================================
// Platform Detection
// ============================================================================

#if defined(_WIN32) || defined(_WIN64)
    #define FLASHDB_WINDOWS 1
#else
    #define FLASHDB_POSIX 1
#endif

// ============================================================================
// Platform-Specific Includes
// ============================================================================

#ifdef FLASHDB_WINDOWS

    // Must define before including winsock2.h to prevent winsock.h conflicts
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #include <winsock2.h>
    #include <ws2tcpip.h>

    // Link against Winsock library (MSVC pragma)
    #pragma comment(lib, "ws2_32.lib")

    #include <csignal>

#else  // POSIX

    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <csignal>
    #include <cerrno>

#endif

#include <cstring>
#include <iostream>
#include <string>

#ifdef FLASHDB_WINDOWS
    #include <basetsd.h>
    using ssize_t = SSIZE_T;
#endif

namespace flashdb {
namespace platform {

// ============================================================================
// Type Aliases
// ============================================================================

#ifdef FLASHDB_WINDOWS
    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
    using socket_t = int;
    constexpr socket_t INVALID_SOCK = -1;
#endif

// ============================================================================
// Initialization / Cleanup
// ============================================================================

/**
 * Initialize platform networking. Must be called once at program start.
 * On Windows, calls WSAStartup(). On POSIX, this is a no-op.
 * Returns true on success.
 */
inline bool initNetworking() {
#ifdef FLASHDB_WINDOWS
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "[Platform] WSAStartup failed: " << result << "\n";
        return false;
    }
    return true;
#else
    // POSIX: ignore SIGPIPE so write() on closed sockets returns error
    // instead of killing the process.
    std::signal(SIGPIPE, SIG_IGN);
    return true;
#endif
}

/**
 * Clean up platform networking. Must be called once at program exit.
 * On Windows, calls WSACleanup(). On POSIX, this is a no-op.
 */
inline void cleanupNetworking() {
#ifdef FLASHDB_WINDOWS
    WSACleanup();
#endif
}

// ============================================================================
// Socket Operations
// ============================================================================

/** Close a socket. */
inline int closeSocket(socket_t sock) {
#ifdef FLASHDB_WINDOWS
    return ::closesocket(sock);
#else
    return ::close(sock);
#endif
}

/** Read from a socket. Returns bytes read, 0 on disconnect, -1 on error. */
inline ssize_t socketRead(socket_t sock, char* buf, size_t len) {
#ifdef FLASHDB_WINDOWS
    return ::recv(sock, buf, static_cast<int>(len), 0);
#else
    return ::read(sock, buf, len);
#endif
}

/** Write to a socket. Returns bytes written, -1 on error. */
inline ssize_t socketWrite(socket_t sock, const char* buf, size_t len) {
#ifdef FLASHDB_WINDOWS
    return ::send(sock, buf, static_cast<int>(len), 0);
#else
    return ::write(sock, buf, len);
#endif
}

/** Check if a socket is valid. */
inline bool isValidSocket(socket_t sock) {
#ifdef FLASHDB_WINDOWS
    return sock != INVALID_SOCKET;
#else
    return sock >= 0;
#endif
}

/** Get the last socket error as a string. */
inline std::string getLastSocketError() {
#ifdef FLASHDB_WINDOWS
    int err = WSAGetLastError();
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, buf, sizeof(buf), nullptr);
    return std::string(buf);
#else
    return std::strerror(errno);
#endif
}

/** Check if the last error was an interrupt (EINTR). */
inline bool wasInterrupted() {
#ifdef FLASHDB_WINDOWS
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

/** Set SO_REUSEADDR on a socket. */
inline bool setReuseAddr(socket_t sock) {
#ifdef FLASHDB_WINDOWS
    const char opt = 1;
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0;
#else
    int opt = 1;
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0;
#endif
}

// ============================================================================
// Signal Handling
// ============================================================================

/**
 * Install platform-appropriate signal handlers.
 * On POSIX: registers SIGINT handler + ignores SIGPIPE.
 * On Windows: registers SIGINT handler (no SIGPIPE on Windows).
 */
inline void installSignalHandler(void (*handler)(int)) {
    std::signal(SIGINT, handler);
#ifdef FLASHDB_POSIX
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

}  // namespace platform
}  // namespace flashdb
