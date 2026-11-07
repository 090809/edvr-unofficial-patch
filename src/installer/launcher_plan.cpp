#include "launcher_plan.h"

#include <windows.h>
#include <shellapi.h>

#include <map>
#include <set>

#include "../common/iniedit.h"
#include "apply.h"
#include "mirror.h"
#include "payload.h"
#include "plan.h"
#include "state.h"
#include "update_http.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

constexpr const char* kPlanVersion = "1";

bool setError(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

bool hasBreak(const std::string& value) { return value.find_first_of("\r\n") != std::string::npos; }

std::string yesNo(bool value) { return value ? "1" : "0"; }

// key=value lines into a map, refusing a repeat and a line with no '='.
bool parseLines(const std::string& text, std::map<std::string, std::string>* out, std::string* error) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) return setError(error, "a line has no key");
        const std::string key = line.substr(0, eq);
        if (key == "line") continue;  // result files repeat it; the caller reads them separately
        if (out->count(key)) return setError(error, "the key " + key + " is given twice");
        (*out)[key] = line.substr(eq + 1);
    }
    return true;
}

bool boolOf(const std::map<std::string, std::string>& m, const char* key, bool* out, std::string* error) {
    const auto found = m.find(key);
    if (found == m.end()) return setError(error, std::string("the key ") + key + " is missing");
    if (found->second != "0" && found->second != "1") return setError(error, std::string("the key ") + key + " is not 0 or 1");
    *out = found->second == "1";
    return true;
}

std::string textOf(const std::map<std::string, std::string>& m, const char* key) {
    const auto found = m.find(key);
    return found == m.end() ? std::string() : found->second;
}

ApplyOutcome failed(const std::string& why) {
    ApplyOutcome o;
    o.status = ApplyOutcome::Status::Failed;
    o.lines.push_back(why);
    return o;
}

const char* statusName(ApplyOutcome::Status s) {
    switch (s) {
        case ApplyOutcome::Status::Done: return "done";
        case ApplyOutcome::Status::NothingToDo: return "nothing";
        case ApplyOutcome::Status::Blocked: return "blocked";
        default: return "failed";
    }
}

// The plan's work, as outcome. Everything the elevated copy does after reading the file.
ApplyOutcome runPlan(const PlanFile& file, const char* const* keys) {
    if (file.action != "install" && file.action != "repair" && file.action != "uninstall")
        return failed("the plan names an action this launcher does not know");
    if (file.dir.empty()) return failed("the plan names no game folder");

    // The payload: re-verified here, not trusted from the file.
    std::string error;
    if (!file.payloadDir.empty()) {
        std::vector<PayloadFile> files;
        std::string version;
        if (!listPayloadFolder(file.payloadDir, &files, &version, &error) || !usePayloadFiles(files, version, &error))
            return failed("the payload folder cannot be used: " + error);
    } else if (!file.manifestPath.empty()) {
        OfflineClient offline;
        UpdaterSettings settings;
        settings.cacheDir = file.cacheDir;
        settings.keys = keys;
        Updater updater(offline, settings);
        Manifest manifest;
        std::vector<PayloadFile> files;
        if (!updater.loadManifestFile(file.manifestPath, &manifest, &files, &error) ||
            !usePayloadFiles(files, manifest.build, &error))
            return failed("the downloaded files cannot be used: " + error);
    } else if (file.action != "uninstall") {
        return failed("the plan names no payload to install from");
    }

    GameInstall game;
    if (!describeDir(file.dir, file.source.empty() ? L"Folder you chose" : file.source, &game))
        return failed("there is no EliteDangerous64.exe in the game folder the plan names");

    Survey survey = surveyTarget(game);
    const bool uninstall = file.action == "uninstall";
    ApplyOutcome outcome;

    // The restore the user agreed to in the window; it writes into the game folder, so it
    // is done here, with the rights the rest of the work has.
    if (file.restoreMirror && file.keepSettings && !uninstall) {
        const MirrorInfo mirror = readMirror(file.mirrorDir);
        const std::string scope = file.scope.empty() ? defaultScope(survey) : file.scope;
        if (!settingsToRestore(survey.game.dir, mirror, scope).empty()) {
            std::vector<std::string> notes;
            const bool restored = restoreFromMirror(survey.game.dir, mirror, scope, &notes);
            for (const std::string& n : notes) outcome.lines.push_back(n);
            if (!restored) outcome.lines.push_back("Not all of the saved settings could be restored; carrying on with what is in the folder.");
            survey = surveyTarget(game);
        }
    }

    Options options;
    options.scope = file.scope;
    options.keepSettings = file.keepSettings;
    options.removeSettings = file.removeSettings;
    options.repair = file.action == "repair";
    options.backupStamp = timestampName();
    options.nowUtc = utcNow();
    const Plan plan = uninstall ? planUninstall(survey, options, payloadInfo()) : planInstall(survey, options, payloadInfo());

    if (plan.blocked) {
        ApplyOutcome blocked;
        blocked.status = ApplyOutcome::Status::Blocked;
        for (const std::string& p : plan.problems) blocked.lines.push_back(p);
        if (blocked.lines.empty()) blocked.lines.push_back(planSummary(plan));
        return blocked;
    }
    if (plan.nothingToDo) {
        if (!uninstall) updateMirror(survey.game.dir, plan.backupDir, file.mirrorDir);
        outcome.status = ApplyOutcome::Status::NothingToDo;
        outcome.lines.push_back(planSummary(plan));
        return outcome;
    }

    const ApplyResult result = applyPlan(plan, payloadItem);
    for (const std::string& line : result.done) outcome.lines.push_back(line);
    if (!result.ok) {
        outcome.status = ApplyOutcome::Status::Failed;
        outcome.lines.push_back(result.error);
        if (result.rolledBack) outcome.lines.push_back("Installed files and metadata were restored; recovery backups were kept.");
        return outcome;
    }
    if (!uninstall) {
        const MirrorResult m = updateMirror(survey.game.dir, plan.backupDir, file.mirrorDir);
        if (m.ok) {
            std::string saved;
            for (size_t i = 0; i < m.saved.size(); ++i) saved += (i ? ", " : "") + m.saved[i];
            outcome.lines.push_back("Mirrored " + saved + " to " + narrowUtf8(file.mirrorDir) + ".");
        }
    }
    outcome.status = ApplyOutcome::Status::Done;
    return outcome;
}

}  // namespace

bool writePlanFile(const std::wstring& path, const PlanFile& p, std::string* error) {
    const std::string dir = narrowUtf8(p.dir), source = narrowUtf8(p.source), payloadDir = narrowUtf8(p.payloadDir),
                      manifest = narrowUtf8(p.manifestPath), cache = narrowUtf8(p.cacheDir),
                      mirror = narrowUtf8(p.mirrorDir), result = narrowUtf8(p.resultPath);
    for (const std::string* v : {&p.action, &p.scope, &dir, &source, &payloadDir, &manifest, &cache, &mirror, &result}) {
        if (hasBreak(*v)) return setError(error, "a value in the plan has a line break in it");
    }
    std::string text;
    text += std::string("version=") + kPlanVersion + "\n";
    text += "action=" + p.action + "\n";
    text += "dir=" + dir + "\n";
    text += "source=" + source + "\n";
    text += "scope=" + p.scope + "\n";
    text += "keep_settings=" + yesNo(p.keepSettings) + "\n";
    text += "remove_settings=" + yesNo(p.removeSettings) + "\n";
    text += "restore_mirror=" + yesNo(p.restoreMirror) + "\n";
    text += "payload_dir=" + payloadDir + "\n";
    text += "manifest=" + manifest + "\n";
    text += "cache_dir=" + cache + "\n";
    text += "mirror_dir=" + mirror + "\n";
    text += "result=" + result + "\n";
    std::wstring why;
    if (!writeFileAtomic(path, text, &why)) return setError(error, "the plan file cannot be written");
    return true;
}

bool readPlanFile(const std::wstring& path, PlanFile* plan, std::string* error) {
    std::string text;
    if (!readFileBytes(path, &text, 1u << 16, error)) return false;
    std::map<std::string, std::string> m;
    if (!parseLines(text, &m, error)) return false;
    static const std::set<std::string> known = {"version",     "action",         "dir",        "source",
                                                "scope",       "keep_settings",  "remove_settings",
                                                "restore_mirror", "payload_dir", "manifest",   "cache_dir",
                                                "mirror_dir",  "result"};
    for (const auto& entry : m) {
        if (!known.count(entry.first)) return setError(error, "the plan has a key this launcher does not know: " + entry.first);
    }
    if (textOf(m, "version") != kPlanVersion) return setError(error, "the plan is a version this launcher does not read");
    PlanFile p;
    p.action = textOf(m, "action");
    p.dir = widenUtf8(textOf(m, "dir"));
    p.source = widenUtf8(textOf(m, "source"));
    p.scope = textOf(m, "scope");
    p.payloadDir = widenUtf8(textOf(m, "payload_dir"));
    p.manifestPath = widenUtf8(textOf(m, "manifest"));
    p.cacheDir = widenUtf8(textOf(m, "cache_dir"));
    p.mirrorDir = widenUtf8(textOf(m, "mirror_dir"));
    p.resultPath = widenUtf8(textOf(m, "result"));
    if (!boolOf(m, "keep_settings", &p.keepSettings, error) || !boolOf(m, "remove_settings", &p.removeSettings, error) ||
        !boolOf(m, "restore_mirror", &p.restoreMirror, error))
        return false;
    if (p.action.empty() || p.dir.empty() || p.resultPath.empty()) return setError(error, "the plan lacks an action, a folder or a result path");
    if (!p.scope.empty() && p.scope != "both" && p.scope != "vr" && p.scope != "flat") return setError(error, "the plan names an unknown scope");
    *plan = std::move(p);
    return true;
}

bool writeResultFile(const std::wstring& path, const ApplyOutcome& outcome, std::string* error) {
    std::string text = std::string("status=") + statusName(outcome.status) + "\n";
    for (const std::string& line : outcome.lines) {
        std::string flat;
        for (char c : line) flat.push_back(c == '\r' || c == '\n' ? ' ' : c);
        text += "line=" + flat + "\n";
    }
    if (!writeFileAtomic(path, text)) return setError(error, "the result file cannot be written");
    return true;
}

bool readResultFile(const std::wstring& path, ApplyOutcome* outcome, std::string* error) {
    std::string text;
    if (!readFileBytes(path, &text, 1u << 20, error)) return false;
    ApplyOutcome out;
    bool haveStatus = false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 7, "status=") == 0) {
            const std::string name = line.substr(7);
            if (name == "done") out.status = ApplyOutcome::Status::Done;
            else if (name == "nothing") out.status = ApplyOutcome::Status::NothingToDo;
            else if (name == "blocked") out.status = ApplyOutcome::Status::Blocked;
            else if (name == "failed") out.status = ApplyOutcome::Status::Failed;
            else return setError(error, "the result names a status this launcher does not know");
            haveStatus = true;
        } else if (line.compare(0, 5, "line=") == 0) {
            out.lines.push_back(line.substr(5));
        } else if (!line.empty()) {
            return setError(error, "the result has a line this launcher does not know");
        }
    }
    if (!haveStatus) return setError(error, "the result has no status");
    *outcome = std::move(out);
    return true;
}

std::string outcomeReport(const ApplyOutcome& outcome) {
    std::string text;
    for (const std::string& line : outcome.lines) text += "  " + line + "\r\n";
    switch (outcome.status) {
        case ApplyOutcome::Status::Done: text += "\r\nDone.\r\n"; break;
        case ApplyOutcome::Status::NothingToDo: text += "\r\nNothing needed changing.\r\n"; break;
        case ApplyOutcome::Status::Blocked: text += "\r\nNothing was changed: the plan is blocked.\r\n"; break;
        default: text += "\r\nThe install did not finish.\r\n"; break;
    }
    return text;
}

int applyPlanFile(const std::wstring& planPath, const char* const* keys) {
    PlanFile plan;
    std::string error;
    // An unreadable plan has no result path to report to, so the parent sees no result file.
    if (!readPlanFile(planPath, &plan, &error)) return 2;
    const ApplyOutcome outcome = runPlan(plan, keys);
    writeResultFile(plan.resultPath, outcome, nullptr);
    return outcome.status == ApplyOutcome::Status::Done || outcome.status == ApplyOutcome::Status::NothingToDo ? 0 : 1;
}

ElevatedLaunch runWithUac(const std::wstring& exePath, const std::wstring& arguments) {
    ElevatedLaunch launch;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exePath.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        launch.declined = code == ERROR_CANCELLED;
        launch.error = launch.declined ? "the permission prompt was declined" : "Windows would not start the elevated copy";
        return launch;
    }
    launch.started = true;
    if (info.hProcess) {
        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(info.hProcess, &exitCode);
        launch.exitCode = exitCode;
        CloseHandle(info.hProcess);
    }
    return launch;
}

ElevatedResult applyElevated(const std::wstring& exePath, const std::wstring& launcherDir, PlanFile plan,
                             const ElevatedRunner& runner) {
    ElevatedResult result;
    const std::wstring stamp = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const std::wstring planPath = joinPath(launcherDir, L"plan-" + stamp + L".txt");
    plan.resultPath = joinPath(launcherDir, L"result-" + stamp + L".txt");
    auto cleanup = [&]() {
        DeleteFileW(planPath.c_str());
        DeleteFileW(plan.resultPath.c_str());
    };
    if (!createDirectories(launcherDir) || !writePlanFile(planPath, plan, &result.error)) {
        if (result.error.empty()) result.error = "the launcher folder cannot be written";
        cleanup();
        return result;
    }
    const ElevatedLaunch launch = runner(exePath, L"--apply-plan " + quoteArg(planPath));
    if (!launch.started) {
        result.declined = launch.declined;
        result.error = launch.error;
        cleanup();
        return result;
    }
    std::string readError;
    if (!readResultFile(plan.resultPath, &result.outcome, &readError)) {
        result.error = "the elevated copy finished without reporting (exit code " + std::to_string(launch.exitCode) + "); nothing is known to have changed";
        result.outcome = failed(result.error);
    }
    cleanup();
    return result;
}

}  // namespace edvr::installer
