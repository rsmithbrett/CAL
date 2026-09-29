#include "CheckInTransport.h"

#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: pi-checkin HTTPS_ORIGIN PRIVATE_CREDENTIAL_FILE PACKAGE_VERSION\n";
    return 2;
  }
  try {
    const auto credentials = PiInstallation::load(argv[2]);
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (!gmtime_r(&now, &utc)) throw std::runtime_error("UTC clock unavailable");
    char timestamp[21];
    if (std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
      throw std::runtime_error("UTC timestamp unavailable");
    const std::string version = argv[3];
    if (version.empty() || version.size() > 64 ||
        version.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos)
      throw std::invalid_argument("invalid package version");
    // Keep the wire compatible with the existing request; a Pi does not
    // invent a battery reading or claim it is charging.
    const std::string body = std::string("{\"deviceUtcTimestamp\":\"") + timestamp +
        "\",\"firmwareVersion\":\"" + version + "\"}";
    const auto reply = PiCheckIn::post(argv[1], credentials, body);
    std::cout << "check-in HTTP " << reply.status << ", response bytes "
              << reply.body.size() << '\n';
    return reply.status == 200 ? 0 : 1;
  } catch (const std::exception& error) {
    // Never print a libcurl URL/header or the secret, even on failure.
    std::cerr << "pi-checkin: " << error.what() << '\n';
    return 1;
  }
}
