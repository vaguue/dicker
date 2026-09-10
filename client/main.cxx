#include "dicker.h"

int main() {
  dicker::Conn conn{"dicker://kek@127.0.0.1:5959"};

  dicker::Config cfg{conn};
  dicker::Scheduler dicker{cfg};

  dicker.addRoot("/Users/seva/Library/Application Support/Telegram Desktop");

  dicker.start();

  return 0;
}
