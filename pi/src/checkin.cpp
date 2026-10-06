#include "CheckInTransport.h"
#include "PolicyState.h"

#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>

// Test evidence stays in an owner-only file, never in console/CI logs. Refuse
// existing paths and symlinks so a requested output cannot overwrite a credential.
static void savePrivateReply(const std::string& path, const std::string& body) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("could not create private response file");
  size_t written = 0;
  while (written < body.size()) {
    const ssize_t count = write(fd, body.data() + written, body.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      close(fd);
      unlink(path.c_str());
      throw std::runtime_error("could not write private response file");
    }
    written += static_cast<size_t>(count);
  }
  if (close(fd) != 0) {
    unlink(path.c_str());
    throw std::runtime_error("could not finish private response file");
  }
}

int main(int argc, char** argv) {
  if (argc != 4 && !(argc == 6 && std::string(argv[4]) == "--response-file")) {
    std::cerr << "usage: pi-checkin HTTPS_ORIGIN PRIVATE_CREDENTIAL_FILE PACKAGE_VERSION [--response-file NEW_PRIVATE_FILE]\n";
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
    if (reply.status == 200) {
      const auto policy = PiPolicy::decode(reply.body);
      if (argc == 6) savePrivateReply(argv[5], reply.body);
      std::cout << "policy response validated, " << (policy ? policy->cards.size() : 0)
                << " entries, present " << (policy ? "yes" : "no") << '\n';
    }
    std::cout << "check-in HTTP " << reply.status << ", response bytes "
              << reply.body.size() << '\n';
    return reply.status == 200 ? 0 : 1;
  } catch (const std::exception& error) {
    // Never print a libcurl URL/header or the secret, even on failure.
    std::cerr << "pi-checkin: " << error.what() << '\n';
    return 1;
  }
}
