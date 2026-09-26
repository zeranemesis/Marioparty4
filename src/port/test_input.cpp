// Scripted pad input for automated tests, off unless PARTYBOARD_TEST_INPUT names a port.
//
// A test driver connects to 127.0.0.1:<port> (on Android: `adb forward tcp:<port> tcp:<port>`;
// PartyBoardActivity only passes the variable on in debuggable builds) and sends lines:
//
//   <buttons> <stickX> <stickY> <frames>
//
// buttons is the PADStatus bit mask in hex (A=100, B=200, X=400, Y=800, START=1000,
// Z=10, R=20, L=40, D-pad 1/2/4/8), sticks are -128..127, and the state is held on pad 1
// for that many frames. Commands queue up; an empty queue leaves the pad to the player.
// Every command is answered with one line once its frames have run, so a driver can wait.

#include "port/test_input.h"

#include <dolphin/pad.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

struct Command {
    PADStatus status {};
    int frames = 0;
};

#ifndef _WIN32
int gListen = -1;
int gClient = -1;
bool gTried = false;
std::string gPending;
std::deque<Command> gQueue;

void set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

void open_listener()
{
    gTried = true;
    const char *value = std::getenv("PARTYBOARD_TEST_INPUT");
    const int port = value != nullptr ? std::atoi(value) : 0;
    if (port <= 0 || port > 65535) {
        return;
    }
    gListen = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (gListen < 0) {
        return;
    }
    const int yes = 1;
    setsockopt(gListen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(gListen, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(gListen, 1) != 0) {
        close(gListen);
        gListen = -1;
        return;
    }
    set_nonblocking(gListen);
    std::fprintf(stderr, "Test input listening on 127.0.0.1:%d\n", port);
}

void read_commands()
{
    if (gClient < 0) {
        gClient = accept(gListen, nullptr, nullptr);
        if (gClient < 0) {
            return;
        }
        set_nonblocking(gClient);
        gPending.clear();
    }
    char buffer[512];
    for (;;) {
        const ssize_t n = recv(gClient, buffer, sizeof(buffer), 0);
        if (n == 0) {
            close(gClient);
            gClient = -1;
            return;
        }
        if (n < 0) {
            break;
        }
        gPending.append(buffer, static_cast<size_t>(n));
    }
    std::size_t end;
    while ((end = gPending.find('\n')) != std::string::npos) {
        const std::string line = gPending.substr(0, end);
        gPending.erase(0, end + 1);
        unsigned buttons = 0;
        int x = 0, y = 0, frames = 1;
        if (std::sscanf(line.c_str(), "%x %d %d %d", &buttons, &x, &y, &frames) >= 1) {
            Command command;
            command.status.button = static_cast<u16>(buttons);
            command.status.stickX = static_cast<s8>(x);
            command.status.stickY = static_cast<s8>(y);
            command.status.analogA = (buttons & PAD_BUTTON_A) ? 0xFF : 0;
            command.status.analogB = (buttons & PAD_BUTTON_B) ? 0xFF : 0;
            command.status.triggerLeft = (buttons & PAD_TRIGGER_L) ? 0xFF : 0;
            command.status.triggerRight = (buttons & PAD_TRIGGER_R) ? 0xFF : 0;
            command.frames = frames < 1 ? 1 : frames;
            gQueue.push_back(command);
        }
    }
}
#endif

} // namespace

extern "C" void PartyBoard_TestInputFrame(void)
{
#ifndef _WIN32
    if (!gTried) {
        open_listener();
    }
    if (gListen < 0) {
        return;
    }
    read_commands();
    if (gQueue.empty()) {
        return;
    }
    Command &command = gQueue.front();
    PADSetVirtualStatus(PAD_CHAN0, &command.status);
    if (--command.frames <= 0) {
        gQueue.pop_front();
        if (gQueue.empty()) {
            PADClearVirtualStatus(PAD_CHAN0);
        }
        if (gClient >= 0) {
            send(gClient, "ok\n", 3, MSG_NOSIGNAL);
        }
    }
#endif
}
