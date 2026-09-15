#include "Enrollment.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "Config.h"
#include "Identity.h"
#include "Journal.h"
#include "Tls.h"

namespace Enrollment {

Result requestKey(const Service::Discovery& discovery) {
  Result out;

  if (discovery.enrollmentPath.length() == 0) {
    Journal::line("[enroll] discovery named no enrollment path - no key can be asked for");
    return out;
  }

  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    // The reply carries a credential, so an unvalidated connection would hand
    // it to anything able to intercept. No trust source means no request.
    Journal::line("[enroll] Tls::configure refused - no request made, because the reply "
                  "would carry a credential over an unvalidated connection");
    return out;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + discovery.enrollmentPath;
  if (!http.begin(client, url)) {
    Journal::printf("[enroll] http.begin() refused %s", url.c_str());
    return out;
  }
  http.setTimeout(discovery.httpTimeoutMs);
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["macAddress"] = Identity::macAddress();
  String payload;
  serializeJson(body, payload);

  const int status = http.POST(payload);
  if (status != 200 && status != 202 && status != 409) {
    Journal::printf("[enroll] POST returned %d - treated as no answer, will retry", status);
    http.end();
    return out;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Journal::printf("[enroll] response did not parse: %s", err.c_str());
    return out;
  }

  // The wording shown on screen comes from the server rather than from a table
  // compiled in here. CAL cannot be updated to change a sentence.
  out.message = doc["message"] | "";

  const String state = doc["state"] | "";
  if (state == "issued") {
    const String secret = doc["deviceSecret"] | "";
    if (secret.length() == 0) {
      Journal::line("[enroll] server said 'issued' but sent no deviceSecret - nothing "
                    "was stored");
      return out;  // claims issued but sent nothing usable
    }
    Identity::saveSecret(secret);
    out.state = State::Issued;
  } else if (state == "pending") {
    out.state = State::Pending;
  } else if (state == "refused") {
    out.state = State::Refused;
  }

  return out;
}

}  // namespace Enrollment
