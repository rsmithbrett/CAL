// A registration written the way every card module in App/ writes one,
// compiled and not linked, from one source twice: once naming a key policy and
// once leaving it out. ci/test-key-policy.sh builds the second with
// KEY_POLICY_OMITTED defined and requires the compiler to refuse it.
//
// This is the compile-time half of the decision. It does not depend on how the
// descriptor was declared, what it is called, or which file or directory it
// sits in: the argument is part of the registry's own signature, so a call that
// leaves it out is a diagnostic wherever it is written.
//
// See BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md, "The decision is an argument".

#include "Cards.h"

namespace {

String resolver(uint16_t) { return String("a key"); }

}  // namespace

bool registerSomething() {
  Cards::CardSpec spec;
  spec.id = "something";
  spec.listingId = &resolver;
#if defined(KEY_POLICY_OMITTED)
  return Cards::registerCard(spec);
#else
  return Cards::registerCard(spec, Cards::KeyPolicy::Resolves);
#endif
}
