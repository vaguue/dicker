#include <thread>

#include "dicker.h"

int main() { 
  //dicker::Conn conn{"dickers://kek@188.166.46.230:5959"};
  //dicker::Conn conn{"dicker://kek@84.38.185.57:5959"};
  dicker::Conn conn{"dickers://kek@127.0.0.1:5959"};
  //dicker::Conn conn{"dickers://kek@161.104.54.88:5959"};

  dicker::Config cfg{conn};
  cfg.verbose = true;

  std::thread runner([&] {
    dicker::Scheduler dicker{cfg};
    dicker.addRoot("/Users/seva/seva/open-source/dicker/dist");

    //dicker.addRoot("/Users/seva/Library/Application Support/Telegram Desktop/tdata", {
    //  dicker::include("media_cache/version"),
    //  dicker::exclude("media_cache/**"),
    //  dicker::include("cache/version"),
    //  dicker::exclude("cache/**"),
    //});

    dicker.start();
  });

  runner.join();

  std::cout << "[*] Finish" << std::endl;

  return 0;
}
