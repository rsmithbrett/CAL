#pragma once

/// Time sync only - the App has no discovery document to fetch (see Config.h's
/// remarks on kManifestPath for why) and no enrollment concern of its own.
namespace AppService {

/// Same ordering requirement as CAL's Service::synchroniseTime: must happen
/// before any HTTPS call, since certificate validity checking needs a
/// plausible clock.
bool synchroniseTime();

}  // namespace AppService
