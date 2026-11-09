#include "InstallationConfig.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void writeAll(int fd, const std::string& value) {
  const char* data = value.data();
  size_t remaining = value.size();
  while (remaining) {
    const ssize_t count = write(fd, data, remaining);
    assert(count > 0);
    data += count;
    remaining -= static_cast<size_t>(count);
  }
}

bool refused(const std::string& path) {
  try { (void)PiInstallation::load(path); }
  catch (const std::runtime_error&) { return true; }
  return false;
}

}  // namespace

int main() {
  char firstPath[] = "/tmp/dam-pi-first-XXXXXX";
  char secondPath[] = "/tmp/dam-pi-second-XXXXXX";
  const int first = mkstemp(firstPath);
  const int second = mkstemp(secondPath);
  assert(first >= 0 && second >= 0);
  const std::string secretA(43, 'A'), secretB(43, 'B');
  writeAll(first, "installationId=11111111-1111-4111-8111-111111111111\ndeviceSecret=" + secretA + "\n");
  writeAll(second, "installationId=22222222-2222-4222-8222-222222222222\ndeviceSecret=" + secretB + "\n");
  close(first);
  close(second);

  const auto a = PiInstallation::load(firstPath);
  const auto b = PiInstallation::load(secondPath);
  assert(a.installationId != b.installationId && a.deviceSecret != b.deviceSecret);
  assert(a.deviceSecret == secretA && b.deviceSecret == secretB);
  assert(chmod(firstPath, 0644) == 0 && refused(firstPath));
  assert(chmod(firstPath, 0600) == 0);

  const int malformed = open(secondPath, O_WRONLY | O_TRUNC);
  assert(malformed >= 0);
  writeAll(malformed, "installationId=22222222-2222-4222-8222-222222222222\ndeviceSecret=" + secretB + "\nextra=1\n");
  close(malformed);
  assert(refused(secondPath));

  const std::string linkPath = std::string(firstPath) + "-link";
  assert(symlink(firstPath, linkPath.c_str()) == 0 && refused(linkPath));
  unlink(linkPath.c_str());
  unlink(firstPath);
  unlink(secondPath);
  assert(refused(firstPath));
  std::cout << "installation config: private, distinct credentials and refusal checks passed\n";
}
