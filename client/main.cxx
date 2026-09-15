#include <thread>

#include "dicker.h"

int main() { 
  //dicker::Conn conn{"dickers://kek@84.38.185.57:5959"};
  dicker::Conn conn{"dickers://kek@127.0.0.1:5959"};

  dicker::Config cfg{conn};
  cfg.verbose = true;

  std::thread runner([&] {
    dicker::Scheduler dicker{cfg};
    dicker.addRoot("/Users/seva/seva/ctf/boynextdoor/dicker/dist");
    dicker.start();
  });

  runner.join();

  std::cout << "[*] Finish" << std::endl;

  return 0;
}
