#include <cstring>

#include "scheduler.h"

int main() {
  Conn conn{};
  conn.host = "127.0.0.1";
  conn.port = "5959";
  conn.key = "kek";
  conn.flags = 0;
  std::memcpy(conn.sessionId.data(), "123456789123456", 15);

  Config cfg{conn};
  Scheduler dicker{cfg};

  dicker.addRoot("/Users/seva/seva/ctf/crusader/dicker/client");

  dicker.start();

  return 0;
}
