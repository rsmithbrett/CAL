#pragma once

#include "InstallationConfig.h"

#include <curl/curl.h>

#include <stdexcept>
#include <string>

namespace PiCheckIn {

struct Reply {
  long status = 0;
  std::string body;
};

// Only the transport boundary: response policy and actions are handled by the
// client state machine in a later slice. Never log the request headers or body.
inline Reply post(const std::string& baseUrl,
                  const PiInstallation::Credentials& credentials,
                  const std::string& requestJson) {
  bool testLoopback = false;
#ifdef PI_CHECKIN_TEST_LOOPBACK
  // Compiled only into the integration-test binary. Never permit non-TLS
  // traffic to a hostname (including localhost, which can be remapped).
  testLoopback = baseUrl.compare(0, 17, "http://127.0.0.1:") == 0 &&
      baseUrl.size() > 17 &&
      baseUrl.find_first_not_of("0123456789", 17) == std::string::npos;
#endif
  if ((!testLoopback && (baseUrl.compare(0, 8, "https://") != 0 ||
       baseUrl.find_first_of("/?#@ \t\r\n", 8) != std::string::npos ||
       baseUrl.size() <= 8)) ||
      (testLoopback && baseUrl.find_first_of("/?#@ \t\r\n", 7) != std::string::npos)) {
    throw std::invalid_argument("check-in requires an HTTPS server origin");
  }
  if (!PiInstallation::validUuid(credentials.installationId) ||
      !PiInstallation::validSecret(credentials.deviceSecret)) {
    throw std::invalid_argument("invalid installation credentials");
  }
  if (requestJson.empty() || requestJson.size() > 16384)
    throw std::invalid_argument("invalid check-in request size");

  CURL* handle = curl_easy_init();
  if (!handle) throw std::runtime_error("could not initialize check-in transport");
  curl_slist* headers = nullptr;
  Reply reply;
  try {
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!headers) throw std::runtime_error("could not allocate check-in headers");
    const std::string auth = "X-Device-Secret: " + credentials.deviceSecret;
    curl_slist* added = curl_slist_append(headers, auth.c_str());
    if (!added) throw std::runtime_error("could not allocate check-in auth header");
    headers = added;

    const std::string url = baseUrl + "/api/checkin";
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, testLoopback ? "http" : "https");
#else
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS, testLoopback ? CURLPROTO_HTTP : CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, requestJson.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(requestJson.size()));
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION,
                     +[](char* data, size_t size, size_t count, void* context) -> size_t {
                       auto* body = static_cast<std::string*>(context);
                       if (size && count > 65536 / size) return 0;
                       const size_t bytes = size * count;
                       if (body->size() + bytes > 65536) return 0;
                       body->append(data, bytes);
                       return bytes;
                     });
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &reply.body);
    const CURLcode result = curl_easy_perform(handle);
    if (result != CURLE_OK) throw std::runtime_error("check-in transport failed");
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &reply.status);
  } catch (...) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    throw;
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(handle);
  return reply;
}

}  // namespace PiCheckIn
