// What the launcher's window does besides installing: loading the payload, asking GitHub
// about an update, fetching it, switching to it, and starting and watching the game.
//
// Everything slow runs on a worker thread and reports back through `post`, which runs a
// job on the window's thread: the window never waits, and nothing here touches a control.
// All state lives on that one thread. The class owns no HWND, so tools\launcher_test drives
// it with a queue standing in for the message loop, a scripted GitHub, and fake programs.
//
// Nothing downloads or changes until the user presses Update (beginUpdate), and a failure to
// reach GitHub is one quiet line, never a dialog.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "direct_launch.h"
#include "launch_route.h"
#include "launcher_core.h"
#include "launcher_plan.h"
#include "selfupdate.h"
#include "updater.h"

namespace edvr::installer {

// What the window asks before it turns direct launch on. No is the default button, and a No leaves it off.
inline constexpr const char* kDirectLaunchQuestion =
    "Start Elite directly instead of through Frontier's launcher?\n\n"
    "The launcher signs in to Frontier's servers itself with your Steam login, then starts the game. "
    "It starts faster, and you can pick VR or flat here. No password or token is stored.\n\n"
    "Frontier has not approved this. It could act against an account that signs in this way, and the choice "
    "is yours. Untick this at any time to go back to Frontier's launcher.\n\n"
    "Turn it on?";

enum class PayloadState { Loading, Ready, None };
enum class CheckState { NotYet, Checking, UpToDate, Available, Refused, Skipped };

struct SessionEnv {
    LauncherRoots roots;
    std::wstring payloadDir;       // --payload, or empty
    std::string  channelOverride;  // --channel for this run, or empty to read launcher.ini
    bool         updatedFlag = false;  // started by a self-update: carry on into the update
    std::wstring selfExe;          // the running exe: hashed for the check, replaced by self-update
    const char* const* keys = nullptr;  // null means the release keys
    std::string  apiBase;          // empty means GitHub

    // Test seams. Empty means the real one.
    std::function<std::unique_ptr<HttpClient>()> makeHttp;
    FileMover move;
    std::function<bool(const std::wstring& exe)> startNewLauncher;

    // The window's side. `post` runs a job on the window's thread.
    std::function<void(std::function<void()>)> post;
    std::function<void()> changed;          // state moved: repaint, re-enable
    std::function<void()> payloadSwitched;  // an update was adopted and the payload is now it: carry on to the install
    std::function<void()> closeWindow;      // the new launcher is running: this one is done
    std::function<void(bool aside)> windowAside;  // the game started (true) or is over (false)
    // A direct launch hides the window while the game runs (windowAside's minimising is the fallback
    // when this is empty) and closes it when the game exits.
    std::function<void()> hideWindow;

    // Direct launch's seams. Empty or default means the real one; the rig uses its own.
    std::string frontierBase;                  // empty means api.zaonce.net
    std::function<std::unique_ptr<SteamSession>(const std::wstring& dllPath)> makeSteam;
    MachineIdKeys machineKeys;
    ApiClock clock;
    GameStarter startGame;                     // returns the process to wait on; empty means startGameProcess
};

class LauncherSession {
public:
    explicit LauncherSession(SessionEnv env);
    ~LauncherSession() { shutdown(); }
    LauncherSession(const LauncherSession&) = delete;
    LauncherSession& operator=(const LauncherSession&) = delete;

    // Loads the payload and checks for an update, both on worker threads.
    void start();
    // Cancels what is running and waits for the workers. Leaves the game and EDLaunch alone.
    void shutdown();

    PayloadState payloadState() const { return payloadState_; }
    const PayloadLoad& payload() const { return payload_; }

    CheckState checkState() const { return checkState_; }
    const UpdateCheck& check() const { return check_; }
    // The update line: "Checking...", "ED-UP 0.23.0 is available.", progress, or the quiet reason.
    const std::string& updateLine() const { return updateLine_; }
    bool updating() const { return updating_; }

    bool includeReleaseCandidates() const { return channel_ == "rc"; }
    // Saves the channel to launcher.ini and asks again.
    void setIncludeReleaseCandidates(bool include);

    // The Update button. Self-update first if the manifest wants a newer launcher (and this is
    // the canonical copy), restarting; then the cache, the manifest and the payload.
    void beginUpdate();

    // Starts the game and watches until it is over. `elevated` refuses, before anything is contacted.
    // A Steam install with direct launch on signs in to Frontier and starts the game itself, and falls
    // back to Steam's route, saying why, when it cannot. Every other install takes its route.
    void beginPlay(const std::wstring& source, const std::wstring& gameDir, bool vr, bool elevated,
                   const RouteOps& ops, const WatchNames& names, const WatchTiming& timing);
    bool playing() const { return playing_; }
    const std::string& playLine() const { return playLine_; }

    // launcher.ini's direct_launch: off unless it says on. Turning it on calls `confirm` first (the
    // window's question), and a false from it leaves the setting as it was. Returns the setting after.
    bool directLaunch() const { return directLaunch_; }
    bool setDirectLaunch(bool on, const std::function<bool()>& confirm);

    // The elevated apply, waited for on a worker. `done` runs on the window's thread.
    void beginElevatedApply(const PlanFile& plan, const ElevatedRunner& runner,
                            std::function<void(const ElevatedResult&)> done);
    bool applying() const { return applying_; }

private:
    void beginCheck();
    void checkDone(const UpdateCheck& result);
    void updateFailed(const std::string& why);
    void finishSelfUpdate();
    void payloadLoaded(const PayloadLoad& load);
    UpdaterSettings updaterSettings() const;
    std::unique_ptr<HttpClient> makeHttp() const;
    void post(std::function<void()> job) { env_.post(std::move(job)); }
    void changed() {
        if (env_.changed) env_.changed();
    }
    void spawn(std::function<void()> work) { workers_.emplace_back(std::move(work)); }
    void rememberMode(const std::wstring& gameDir, bool vr);
    void startRouteAndWatch(const std::wstring& source, const std::wstring& gameDir, bool vr, bool elevated,
                            const RouteOps& ops, const WatchNames& names, const WatchTiming& timing,
                            const std::string& startLine);
    void beginDirectPlay(const std::wstring& source, const std::wstring& gameDir, bool vr, const RouteOps& ops,
                         const WatchNames& names, const WatchTiming& timing);

    SessionEnv env_;
    std::vector<std::thread> workers_;
    std::atomic<bool> cancel_{false};

    PayloadState payloadState_ = PayloadState::Loading;
    PayloadLoad payload_;
    CheckState checkState_ = CheckState::NotYet;
    UpdateCheck check_;
    std::string updateLine_;
    std::string channel_ = "stable";
    bool updating_ = false;
    bool autoResumed_ = false;  // the update that followed a self-update's restart was started
    bool playing_ = false;
    bool directLaunch_ = false;
    bool applying_ = false;
    std::string playLine_;
};

}  // namespace edvr::installer
