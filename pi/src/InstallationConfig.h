#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <string>

namespace PiInstallation {

struct Credentials {
  std::string installationId;
  std::string deviceSecret;
};

inline bool validUuid(const std::string& value) {
  if (value.size() != 36) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!((value[i] >= '0' && value[i] <= '9') ||
                 (value[i] >= 'a' && value[i] <= 'f') ||
                 (value[i] >= 'A' && value[i] <= 'F'))) {
      return false;
    }
  }
  return true;
}

inline bool validSecret(const std::string& value) {
  // The server's GenerateSecret returns 32 random bytes as unpadded base64url.
  if (value.size() != 43) return false;
  for (char c : value) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

inline Credentials load(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) throw std::runtime_error("installation credential file cannot be opened");

  struct stat st {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
    close(fd);
    throw std::runtime_error("installation credential file must be owned by the client and private");
  }

  std::string contents;
  char buffer[256];
  for (;;) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      close(fd);
      throw std::runtime_error("installation credential file cannot be read");
    }
    if (count == 0) break;
    contents.append(buffer, static_cast<size_t>(count));
    if (contents.size() > 256) {
      close(fd);
      throw std::runtime_error("installation credential file is too large");
    }
  }
  close(fd);

  // Deliberately fixed format: no duplicate fields, comments, unknown keys, or
  // whitespace that could make two clients interpret the same file differently.
  const std::string first = "installationId=";
  const std::string second = "deviceSecret=";
  const size_t newline = contents.find('\n');
  if (newline == std::string::npos || contents.compare(0, first.size(), first) != 0 ||
      contents.compare(newline + 1, second.size(), second) != 0) {
    throw std::runtime_error("installation credential file has an invalid format");
  }
  const size_t end = contents.find('\n', newline + 1);
  if (end == std::string::npos || end != contents.size() - 1 ||
      end < newline + 1 + second.size()) {
    throw std::runtime_error("installation credential file has an invalid format");
  }
  Credentials credentials{
      contents.substr(first.size(), newline - first.size()),
      contents.substr(newline + 1 + second.size(), end - newline - 1 - second.size())};
  if (!validUuid(credentials.installationId) || !validSecret(credentials.deviceSecret)) {
    throw std::runtime_error("installation credential file has invalid fields");
  }
  return credentials;
}

}  // namespace PiInstallation
