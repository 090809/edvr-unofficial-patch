#include "launcher_session.h"

#include <windows.h>

#include <cctype>

#include "launch_route.h"
#include "launcher_state.h"
#include "probe.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

// The updater's messages are lower-case fragments with no full stop; the window shows sentences.
std::string sentence(std::string text) {
    if (text.empty()) return text;
    text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    if (text.back() != '.') text += '.';
    return text;
}

std::wstring launcherIniPath(const LauncherRoots& roots) { return joinPath(roots.launcherDir(), L"launcher.ini"); }

// "name 42%" or just the name when the size is not known.
std::string progressText(const std::string& name, unsigned long long received, unsigned long long total) {
    if (total == 0) return "Downloading " + name + "...";
    const unsigned long long percent = received >= total ? 100 : received * 100 / total;
    return "Downloading " + name + "  " + std::to_string(percent) + "%";
}

}  // namespace

std::unique_ptr<HttpClient> LauncherSession::makeHttp() const {
    if (env_.makeHttp) return env_.makeHttp();
    return std::unique_ptr<HttpClient>(new WinHttpClient(false, EDUP_LAUNCHER_VERSION));
}

UpdaterSettings LauncherSession::updaterSettings() const {
    UpdaterSettings s;
    s.launcherDir = env_.roots.launcherDir();
    s.cacheDir = env_.roots.cacheDir();
    s.channel = channel_;
    s.keys = env_.keys;
    if (!env_.apiBase.empty()) s.apiBase = env_.apiBase;
    return s;
}

void LauncherSession::start() {
    LauncherState state;
    if (state.load(launcherIniPath(env_.roots), nullptr)) channel_ = state.channel();
    if (!env_.channelOverride.empty()) channel_ = env_.channelOverride;

    // The payload first: it is what makes Install available. The window is already up.
    payloadState_ = PayloadState::Loading;
    const LauncherRoots roots = env_.roots;
    const std::wstring payloadDir = env_.payloadDir;
    const char* const* keys = env_.keys;
    spawn([this, roots, payloadDir, keys]() {
        const PayloadLoad load = loadLauncherPayload(roots, payloadDir, keys);
        post([this, load]() { payloadLoaded(load); });
    });
    beginCheck();
}

void LauncherSession::shutdown() {
    cancel_ = true;
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

void LauncherSession::payloadLoaded(const PayloadLoad& load) {
    payload_ = load;
    payloadState_ = load.ok ? PayloadState::Ready : PayloadState::None;
    changed();
}

void LauncherSession::beginCheck() {
    checkState_ = CheckState::Checking;
    updateLine_ = "Checking for updates...";
    changed();
    UpdaterSettings settings = updaterSettings();
    const std::wstring self = env_.selfExe;
    spawn([this, settings, self]() mutable {
        std::unique_ptr<HttpClient> http = makeHttp();
        if (!self.empty()) settings.launcherSha256 = sha256File(self);
        Updater updater(*http, settings);
        const UpdateCheck result = updater.check();
        post([this, result]() { checkDone(result); });
    });
}

void LauncherSession::checkDone(const UpdateCheck& result) {
    if (updating_) return;  // an update is under way against the check that offered it
    check_ = result;
    switch (result.outcome) {
        case UpdateOutcome::Available:
            checkState_ = CheckState::Available;
            updateLine_ = "ED-UP " + result.manifest.version + " is available.";
            break;
        case UpdateOutcome::UpToDate:
            // Not always "up to date": a release with no signed update, with nothing downloaded yet, lands here too.
            checkState_ = CheckState::UpToDate;
            updateLine_ = sentence(result.message);
            break;
        case UpdateOutcome::Refused:
            checkState_ = CheckState::Refused;
            updateLine_ = sentence(result.message);
            break;
        default:
            checkState_ = CheckState::Skipped;
            updateLine_ = sentence(result.message);
            break;
    }
    changed();
    // Started by a self-update: the user already pressed Download, so carry on.
    if (env_.updatedFlag && !autoResumed_ && checkState_ == CheckState::Available) {
        autoResumed_ = true;
        beginUpdate();
    }
}

void LauncherSession::setIncludeReleaseCandidates(bool include) {
    channel_ = include ? "rc" : "stable";
    LauncherState state;
    std::string error;
    if (state.load(launcherIniPath(env_.roots), &error)) {
        state.setChannel(channel_);
        createDirectories(env_.roots.launcherDir());
        state.save(&error);
    }
    if (updating_ || checkState_ == CheckState::Checking) return;
    beginCheck();
}

void LauncherSession::updateFailed(const std::string& why) {
    updating_ = false;
    updateLine_ = sentence(why);
    changed();
}

void LauncherSession::beginUpdate() {
    if (checkState_ != CheckState::Available || updating_ || applying_) return;
    const UpdateCheck check = check_;
    const bool canonical = env_.roots.canonical;
    // A launcher that is not the canonical copy never replaces itself; one this old cannot read
    // the new payload, and says so rather than guessing.
    const bool tooOld = compareVersions(EDUP_LAUNCHER_VERSION, check.manifest.minLauncher) < 0;
    if (tooOld && !canonical) {
        updateFailed("this copy of the launcher is too old for that release and does not replace itself when run with --data-root");
        return;
    }
    // After a restart for a self-update the launcher is already what the manifest names, or the
    // update would loop: carry on with the payload either way.
    const bool selfUpdate = check.launcherNeedsUpdate && canonical && !env_.updatedFlag;
    updating_ = true;
    updateLine_ = selfUpdate ? "Updating the launcher..." : "Starting the update...";
    changed();

    const UpdaterSettings settings = updaterSettings();
    const LauncherRoots roots = env_.roots;
    const std::wstring self = env_.selfExe;
    spawn([this, check, settings, roots, selfUpdate, self]() mutable {
        std::unique_ptr<HttpClient> http = makeHttp();
        std::string error;
        // Progress is posted when the whole percent changes, not on every chunk.
        auto lastShown = std::make_shared<std::string>();
        DownloadProgress progress = [this, lastShown](const std::string& name, unsigned long long received,
                                                      unsigned long long total) {
            const std::string text = progressText(name, received, total);
            if (text == *lastShown) return;
            *lastShown = text;
            post([this, text]() {
                if (!updating_) return;
                updateLine_ = text;
                changed();
            });
        };
        if (selfUpdate) {
            if (self.empty() || !downloadLauncher(*http, check, self, &cancel_, progress, &error)) {
                post([this, error]() { updateFailed("could not update the launcher: " + error); });
                return;
            }
            post([this]() { finishSelfUpdate(); });
            return;
        }
        Updater updater(*http, settings);
        if (!updater.downloadUpdate(check, &cancel_, progress, &error) || !updater.commitManifest(check, &error)) {
            post([this, error]() { updateFailed("the update did not finish: " + error); });
            return;
        }
        // The payload the window installs from is now the adopted manifest, not a --payload folder.
        const PayloadLoad load = loadLauncherPayload(roots, std::wstring(), settings.keys);
        post([this, load]() {
            updating_ = false;
            payloadLoaded(load);
            if (!load.ok) {
                updateLine_ = sentence("the update was downloaded but could not be switched to: " + load.note);
                changed();
                return;
            }
            updateLine_ = "ED-UP " + check_.manifest.version + " is ready to install.";
            checkState_ = CheckState::UpToDate;
            changed();
            if (env_.payloadSwitched) env_.payloadSwitched();
        });
    });
}

void LauncherSession::finishSelfUpdate() {
    std::string error;
    const FileMover move = env_.move ? env_.move : FileMover(moveReplacing);
    if (!swapInNewLauncher(env_.selfExe, move, &error)) {
        DeleteFileW(newLauncherPath(env_.selfExe).c_str());
        updateFailed("could not update the launcher: " + error);
        return;
    }
    const bool started = env_.startNewLauncher && env_.startNewLauncher(env_.selfExe);
    if (!started) {
        updating_ = false;
        updateLine_ = "The launcher was updated. Close this window and start ED-UP again to carry on.";
        changed();
        return;
    }
    if (env_.closeWindow) env_.closeWindow();
}

LauncherSession::LauncherSession(SessionEnv env) : env_(std::move(env)) {
    LauncherState state;
    if (state.load(launcherIniPath(env_.roots), nullptr)) directLaunch_ = state.directLaunch();
}

bool LauncherSession::setDirectLaunch(bool on, const std::function<bool()>& confirm) {
    if (on == directLaunch_) return directLaunch_;
    // Only turning it on is a decision to be asked about; a No leaves it exactly as it was.
    if (on && (!confirm || !confirm())) return directLaunch_;
    directLaunch_ = on;
    LauncherState state;
    std::string error;
    if (state.load(launcherIniPath(env_.roots), &error)) {
        state.setDirectLaunch(on);
        createDirectories(env_.roots.launcherDir());
        state.save(&error);
    }
    changed();
    return directLaunch_;
}

void LauncherSession::rememberMode(const std::wstring& gameDir, bool vr) {
    LauncherState state;
    std::string ignored;
    if (state.load(launcherIniPath(env_.roots), &ignored)) {
        state.setMode(gameDir, vr ? "vr" : "flat");
        createDirectories(env_.roots.launcherDir());
        state.save(&ignored);
    }
}

void LauncherSession::beginPlay(const std::wstring& source, const std::wstring& gameDir, bool vr, bool elevated,
                                const RouteOps& ops, const WatchNames& names, const WatchTiming& timing) {
    if (playing_) return;
    if (directLaunch_ && _wcsicmp(source.c_str(), L"Steam") == 0) {
        // Refused before anything is contacted, as the routes are.
        if (elevated) {
            playLine_ = elevatedRefusal();
            changed();
            return;
        }
        beginDirectPlay(source, gameDir, vr, ops, names, timing);
        return;
    }
    startRouteAndWatch(source, gameDir, vr, elevated, ops, names, timing, "Starting Elite...");
}

void LauncherSession::startRouteAndWatch(const std::wstring& source, const std::wstring& gameDir, bool vr, bool elevated,
                                         const RouteOps& ops, const WatchNames& names, const WatchTiming& timing,
                                         const std::string& startLine) {
    const LaunchRoute route = routeFor(source, gameDir, vr);
    std::string error;
    if (!startRoute(route, ops, elevated, &error)) {
        playLine_ = error;
        changed();
        return;
    }
    // The Frontier route is the only one that carried a choice, so only it is remembered.
    if (route.carriesMode) rememberMode(gameDir, vr);
    playing_ = true;
    playLine_ = startLine;
    changed();
    spawn([this, names, timing]() {
        const WatchOutcome outcome = watchLaunch(names, timing, &cancel_, [this](WatchEvent e) {
            post([this, e]() {
                if (e == WatchEvent::WaitingForGame) {
                    playLine_ = "Waiting for Elite to start; if EDLaunch is updating the game, let it finish.";
                } else if (e == WatchEvent::GameSeen) {
                    playLine_ = "Elite is running.";
                    if (env_.windowAside) env_.windowAside(true);
                } else {
                    playLine_ = "Elite has closed.";
                }
                changed();
            });
        });
        post([this, outcome]() {
            playing_ = false;
            switch (outcome.kind) {
                case WatchOutcome::Kind::GameRan:
                    playLine_ = outcome.edlaunchClosed ? "Elite has closed, and so has EDLaunch."
                                                        : "Elite has closed. EDLaunch is still open and was left as it is.";
                    if (env_.windowAside) env_.windowAside(false);
                    break;
                case WatchOutcome::Kind::EdlaunchLeft:
                    playLine_ = "EDLaunch closed before the game started, so there is nothing more to wait for.";
                    break;
                case WatchOutcome::Kind::NeverStarted:
                    playLine_ = "Neither EDLaunch nor Elite started. If you start it from Steam or Epic, check that launcher is running.";
                    break;
                case WatchOutcome::Kind::CouldNotWatch:
                    playLine_ = "Windows would not list the running programs, so the launcher cannot tell when Elite ends.";
                    break;
                default:
                    break;  // cancelled: the window is closing
            }
            changed();
        });
    });
}

// Direct launch (docs\design-launcher-2026-11-07.md, "Direct, on Steam and the Frontier store"): the
// sign-in on a worker, the start, and the wait on the process handle held here. A fallback goes back
// to the window's thread and takes the Steam route, with its reason as the line.
void LauncherSession::beginDirectPlay(const std::wstring& source, const std::wstring& gameDir, bool vr, const RouteOps& ops,
                                      const WatchNames& names, const WatchTiming& timing) {
    rememberMode(gameDir, vr);
    playing_ = true;
    playLine_ = "Signing in to Frontier...";
    changed();
    const std::string base = env_.frontierBase.empty() ? std::string(kFrontierApiBase) : env_.frontierBase;
    spawn([this, source, gameDir, vr, ops, names, timing, base]() {
        std::unique_ptr<HttpClient> http = makeHttp();
        DirectEnv direct;
        direct.http = http.get();
        direct.apiBase = base;
        direct.makeSteam = env_.makeSteam;
        direct.machineKeys = env_.machineKeys;
        direct.clock = env_.clock;
        direct.cancel = &cancel_;
        direct.progress = [this](const std::string& line) {
            post([this, line]() {
                if (!playing_) return;
                playLine_ = line;
                changed();
            });
        };
        DirectRequest request;
        request.productDir = gameDir;
        request.vr = vr;
        DirectPlan plan = planDirectLaunch(direct, request);
        if (cancel_) return;  // the window is closing

        // Whatever went wrong, the game is not running yet: Steam's route is the way on.
        auto fallBack = [this, source, gameDir, vr, ops, names, timing](const std::string& reason) {
            post([this, source, gameDir, vr, ops, names, timing, reason]() {
                playing_ = false;
                startRouteAndWatch(source, gameDir, vr, false, ops, names, timing, fallbackLine(reason));
            });
        };
        if (!plan.start) {
            fallBack(plan.reason);
            return;
        }
        const GameStarter start = env_.startGame ? env_.startGame : GameStarter(startGameProcess);
        HANDLE process = start(plan.exe, plan.args, plan.workDir);
        plan = DirectPlan();  // the tokens are not needed again
        if (!process) {
            fallBack("Elite could not be started directly");
            return;
        }
        post([this]() {
            playLine_ = "Elite is running.";
            if (env_.hideWindow) env_.hideWindow();
            else if (env_.windowAside) env_.windowAside(true);
            changed();
        });
        while (WaitForSingleObject(process, 200) == WAIT_TIMEOUT) {
            if (cancel_) {
                CloseHandle(process);
                return;  // the window is closing; the game is left running
            }
        }
        CloseHandle(process);
        post([this]() {
            playing_ = false;
            playLine_ = "Elite has closed.";
            changed();
            if (env_.closeWindow) env_.closeWindow();
        });
    });
}

void LauncherSession::beginElevatedApply(const PlanFile& plan, const ElevatedRunner& runner,
                                         std::function<void(const ElevatedResult&)> done) {
    if (applying_) return;
    applying_ = true;
    changed();
    const std::wstring exe = env_.selfExe;
    const std::wstring dir = env_.roots.launcherDir();
    spawn([this, plan, runner, done, exe, dir]() {
        const ElevatedResult result = applyElevated(exe, dir, plan, runner);
        post([this, result, done]() {
            applying_ = false;
            changed();
            if (done) done(result);
        });
    });
}

}  // namespace edvr::installer
