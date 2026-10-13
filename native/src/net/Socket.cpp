// STUB — to be implemented (owner: network/transports module).
#include "net/Socket.h"

namespace sc {

WsaInit::WsaInit() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
}
WsaInit::~WsaInit() { WSACleanup(); }

SOCKET connectTcp(const std::string&, int, const ConnectOptions&, std::string& error, const std::atomic<bool>*, std::string*) {
    error = "connectTcp not implemented";
    return INVALID_SOCKET;
}
bool makeSocketPair(SOCKET& a, SOCKET& b, std::string& error) {
    a = b = INVALID_SOCKET;
    error = "makeSocketPair not implemented";
    return false;
}
SOCKET listenTcp(const std::string&, int, AddressFamily, std::string& error, int*) {
    error = "listenTcp not implemented";
    return INVALID_SOCKET;
}
void closeSocket(SOCKET& s) {
    if (s != INVALID_SOCKET) closesocket(s);
    s = INVALID_SOCKET;
}
bool sendAll(SOCKET, std::string_view) { return false; }
void setNonBlocking(SOCKET s, bool nonBlocking) {
    u_long v = nonBlocking ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
}
std::string peerName(SOCKET) { return {}; }

Waker::Waker() : event_(WSACreateEvent()) {}
Waker::~Waker() { WSACloseEvent(event_); }
void Waker::signal() { WSASetEvent(event_); }
void Waker::reset() { WSAResetEvent(event_); }

}  // namespace sc
