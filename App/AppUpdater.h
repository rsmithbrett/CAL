#pragma once

/// Whether a newer build than the one currently running has been published.
///
/// Deliberately much smaller than CAL's Updater: the App only ever needs a
/// yes/no answer to decide whether to call Loader::requestUpdate() - the
/// sha256/size verification and the actual flash write stay exclusively CAL's
/// job (see Loader.h), so this file doesn't duplicate that logic.
namespace AppUpdater {

bool newerVersionAvailable();

}  // namespace AppUpdater
