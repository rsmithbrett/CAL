// Drives Cards::keyPolicyHolds() over all three key policies against a
// descriptor that assigns a resolver and one that does not, and
// Cards::keyPolicyName() over the three values the refusal line prints.
//
// That predicate is the rule Cards::registerCard() applies to the half of a
// registration the compiler cannot read: the argument is required at every call
// site, and whether the descriptor agrees with it is known only once the
// registration runs. The call itself lives in App/CardManager.cpp, which names a
// graphics library and does not compile on a host, so that registerCard() still
// asks is held by ci/check-content-key-resolvers.sh.
//
// See BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md, "The decision is an argument".

#include <cstdio>
#include <cstring>

#include "Cards.h"

namespace {

int gFailures = 0;

void expect(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    gFailures++;
  }
}

String resolver(uint16_t) { return String("a key"); }

}  // namespace

int main() {
  Cards::CardSpec keyed;
  keyed.id = "keyed";
  keyed.listingId = &resolver;

  Cards::CardSpec bare;
  bare.id = "bare";

  expect(Cards::keyPolicyHolds(keyed, Cards::KeyPolicy::Resolves),
         "Resolves holds for a descriptor that assigns a resolver");
  expect(!Cards::keyPolicyHolds(bare, Cards::KeyPolicy::Resolves),
         "Resolves fails for a descriptor that assigns none");

  expect(Cards::keyPolicyHolds(bare, Cards::KeyPolicy::PolicyContent),
         "PolicyContent holds for a descriptor that assigns no resolver");
  expect(!Cards::keyPolicyHolds(keyed, Cards::KeyPolicy::PolicyContent),
         "PolicyContent fails for a descriptor that assigns one");

  expect(Cards::keyPolicyHolds(bare, Cards::KeyPolicy::Keyless),
         "Keyless holds for a descriptor that assigns no resolver");
  expect(!Cards::keyPolicyHolds(keyed, Cards::KeyPolicy::Keyless),
         "Keyless fails for a descriptor that assigns one");

  // A descriptor straight out of its declaration, which is the state a module's
  // is in before it fills one in. Stating Resolves and then forgetting the
  // assignment is the case sportslive was.
  Cards::CardSpec fresh;
  expect(!Cards::keyPolicyHolds(fresh, Cards::KeyPolicy::Resolves),
         "a descriptor nobody filled in does not satisfy Resolves");

  // The compiler's own reading of the same predicate. This is as far as a
  // static_assert reaches: a descriptor written as a constant expression. The
  // seventeen real ones are filled in field by field at static-init and are no
  // such thing, which is why the check below it runs at registration instead.
  static_assert(!Cards::keyPolicyHolds(Cards::CardSpec{}, Cards::KeyPolicy::Resolves),
                "an empty descriptor cannot satisfy Resolves");
  static_assert(Cards::keyPolicyHolds(Cards::CardSpec{}, Cards::KeyPolicy::Keyless),
                "an empty descriptor satisfies Keyless");

  expect(std::strcmp(Cards::keyPolicyName(Cards::KeyPolicy::Resolves), "Resolves") == 0,
         "Resolves names itself in the refusal line");
  expect(std::strcmp(Cards::keyPolicyName(Cards::KeyPolicy::PolicyContent), "PolicyContent") == 0,
         "PolicyContent names itself in the refusal line");
  expect(std::strcmp(Cards::keyPolicyName(Cards::KeyPolicy::Keyless), "Keyless") == 0,
         "Keyless names itself in the refusal line");

  if (gFailures != 0) {
    std::printf("%d case(s) failed\n", gFailures);
    return 1;
  }

  std::printf("PASS: key policy against the descriptor it was stated for\n");
  return 0;
}
