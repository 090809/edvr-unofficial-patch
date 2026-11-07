// The launcher's command line and start-up, on top of app.h. Compiled only with EDUP_LAUNCHER.
#pragma once

#ifdef EDUP_LAUNCHER

#include <string>

#include "app.h"
#include "launcher_core.h"

namespace edvr::installer {

// The roots this run uses: --data-root, else the real %LOCALAPPDATA%\EDVR.
LauncherRoots launcherRootsFor(const AppArgs& args);

// Where the settings mirror folders go: the data root, so --data-root keeps them out of the
// user's real folder too.
std::wstring launcherMirrorRoot(const AppArgs& args);

// Loads the payload the run installs from (--payload, else the adopted manifest). True when
// there is one; otherwise `note` says what to do.
bool launcherPreparePayload(const AppArgs& args, std::string* note);

// The commands that need no window: --check-update, --update and --unregister. Prints through
// the console when there is one, and otherwise (--unregister from Windows' list of programs) in
// one message box at the end. Returns the exit code.
bool launcherHasCommand(const AppArgs& args);
int runLauncherCommand(const AppArgs& args, bool haveConsole);

// --apply-plan: the elevated copy. No window, no console; returns the exit code.
int runApplyPlanFile(const AppArgs& args);

// Before any window: remove the leftovers of a self-update, keep to one window per user, and
// when running from somewhere other than %LOCALAPPDATA%\EDVR\launcher, copy there, register and
// start that copy. False means this process is done and `exitCode` is what to return.
// Does nothing, and returns true, under --data-root.
bool launcherStartup(const AppArgs& args, int* exitCode);

// What start-up wants the window to say (a failed copy, say); empty when there is nothing.
const std::string& launcherStartupNote();

}  // namespace edvr::installer

#endif  // EDUP_LAUNCHER
