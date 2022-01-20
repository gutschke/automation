#include "event.h"
#include "lutron.h"
#include "util.h"

#include <fstream>
#include <iostream>

#include "json.hpp"
using json = nlohmann::json;


int main(int argc, char *argv[]) {
  if (argc > 1) {
    json site("{}"_json);
    const std::string& fname = "site.json";
    {
      std::ifstream ifs(fname);
      if ((ifs.rdstate() & std::ifstream::failbit) == 0) {
        json cfg = json::parse(ifs, nullptr, false, true);
        if (!cfg.is_discarded()) {
          site = std::move(cfg);
        }
      }
    }

    static int i = 1;
    Event event;
    Lutron lutron(
      event,
      site.contains("GATEWAY") ? site["GATEWAY"].get<std::string>() : "",
      site.contains("USER") ? site["USER"].get<std::string>() : "",
      site.contains("PASSWORD") ? site["PASSWORD"].get<std::string>() : "");

    // Iterate over all the commands passed in on the command line. As a
    // very last command, query the current time. This forces all previous
    // commands to finish, if they haven't done so.
    const auto loop = Util::rec([&](auto&& loop) -> void {
      std::cout << argv[i] << std::endl;
      lutron.command(i == argc ? "?SYSTEM,1" : argv[i],
        [&](const std::string& result) {
        if (!result.empty()) {
          std::cout << result << std::endl;
        }
        if (++i > argc) {
          // We are done. Cause the main program to exit normally.
          event.exitLoop();
        } else {
          // There are more commands to execute, keep looping.
          loop();
        }
      },
      // If anything goes wrong (e.g. connection closed), exit immediately.
      [&]() { event.exitLoop(); });
    });
    lutron
      .oninit([&](auto cb) {
        // Start looping over commands as soon as connection is ready.
        cb(); loop();
      })
      .oninput([](const std::string& line) {
        // Print all progress messages, but omit login handshake.
        if (!line.empty() && line.find(':') == std::string::npos) {
          if (!line.empty()) {
            std::cout << line << std::endl;
          }
        }
      })
      .onclosed([&]() { event.exitLoop(); });
    // Send an empty command to open the connection.
    lutron.command("");
    event.loop();
  }
  return 0;
}
