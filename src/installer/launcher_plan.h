// Applying an install from a short-lived elevated copy of the launcher.
//
// The launcher itself never runs elevated: the game must never start from an elevated
// process, and a window that can start the game has no business holding administrator
// rights. When a game folder needs them (it is usually under Program Files) the launcher
// writes what it decided to a plan file, starts a copy of itself with `runas` and
// `--apply-plan <file>`, waits, and reads the result file the copy wrote.
//
// The elevated copy shows no window and trusts nothing the file says beyond the choices it
// records. It re-verifies the manifest's signature and every payload file's hash itself,
// surveys the folder again, plans again and applies that plan.
//
// Both files are one `key=value` per line in UTF-8, written atomically. A line it does not
// know, a value with a line break in it, or a missing key makes the reader refuse the file.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "launcher_core.h"

namespace edvr::installer {

struct PlanFile {
    std::string  action;                 // install | repair | uninstall
    std::wstring dir;                    // the game folder
    std::wstring source;                 // how it was found ("Steam", "Epic", ...), for the planner
    std::string  scope;                  // both | vr | flat, or empty for the folder's own
    bool         keepSettings = true;
    bool         removeSettings = false;
    bool         restoreMirror = false;  // restore settings from the mirror first, as the user agreed
    std::wstring payloadDir;             // a --payload folder, or empty
    std::wstring manifestPath;           // the stored manifest (<sha>.json), or empty
    std::wstring cacheDir;               // the payload cache, with manifestPath
    std::wstring mirrorDir;              // this install's settings mirror folder
    std::wstring resultPath;             // where the copy writes its result
};

bool writePlanFile(const std::wstring& path, const PlanFile& plan, std::string* error);
bool readPlanFile(const std::wstring& path, PlanFile* plan, std::string* error);

struct ApplyOutcome {
    enum class Status { Done, NothingToDo, Blocked, Failed };
    Status status = Status::Failed;
    std::vector<std::string> lines;  // one per completed step, or the reasons
};

bool writeResultFile(const std::wstring& path, const ApplyOutcome& outcome, std::string* error);
bool readResultFile(const std::wstring& path, ApplyOutcome* outcome, std::string* error);

// The outcome as the report pane shows it.
std::string outcomeReport(const ApplyOutcome& outcome);

// What the elevated copy does. `keys` null means the release keys; the rig passes its own.
// Never throws, never shows anything: every failure is an outcome. Returns the process
// exit code for --apply-plan: 0 done or nothing to do, 1 blocked or failed, 2 unreadable plan.
int applyPlanFile(const std::wstring& planPath, const char* const* keys);

// How the elevated copy is started and waited for. The real one is ShellExecuteEx with
// `runas`; the rig starts the same program unelevated.
struct ElevatedLaunch {
    bool started = false;
    bool declined = false;     // the user said no at the UAC prompt
    unsigned long exitCode = 0;
    std::string error;
};
using ElevatedRunner = std::function<ElevatedLaunch(const std::wstring& exePath, const std::wstring& arguments)>;
ElevatedLaunch runWithUac(const std::wstring& exePath, const std::wstring& arguments);

struct ElevatedResult {
    bool declined = false;  // nothing changed, and the prompt was refused
    ApplyOutcome outcome;
    std::string error;      // when the copy never reported
};

// The unelevated half: writes the plan (with its result path set) into `launcherDir`, runs
// `runner`, reads the result, and deletes both files whatever happened.
ElevatedResult applyElevated(const std::wstring& exePath, const std::wstring& launcherDir, PlanFile plan,
                             const ElevatedRunner& runner);

}  // namespace edvr::installer
