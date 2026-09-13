#include "dicker.h"

int main() {
  dicker::Conn conn{"dicker://kek@127.0.0.1:5959"};

  dicker::Config cfg{conn};
  dicker::Scheduler dicker{cfg};

  // SDK form of: --include 'media_cache/version' --exclude 'media_cache/**'
  // (ordered, rsync first-match-wins: keep the cache's version marker, drop the
  // rest of the media cache, keep everything else).
  dicker.addRoot("/Users/seva/Library/Application Support/Telegram Desktop", {
    dicker::include("media_cache/version"),
    dicker::exclude("media_cache/**"),
  });

  dicker.start();

  return 0;
}
