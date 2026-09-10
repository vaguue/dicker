#include "scheduler.h"

int main() {
  // sessionId defaults to a random 16 bytes (see conn.h); set conn.sessionId to override.
  Conn conn = parseConn("dicker://kek@127.0.0.1:5959");

  Config cfg{conn};
  Scheduler dicker{cfg};

  dicker.addRoot("/Users/seva/Library/Application Support/Telegram Desktop");

  dicker.start();

  return 0;
}
