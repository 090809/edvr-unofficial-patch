// Starting Elite without EDLaunch: the sign-in, and the command line the game is given.
// A port of the launch path of min-ed-launcher (https://github.com/rfvgyhn/min-ed-launcher, commit
// 139e4ff7ab9027925648fb27767cb87acdcf290a, MIT licence, "Copyright (c) 2017 Chris"; the notice is
// third_party\min_ed_launcher\LICENSE): Product.fs (VersionInfo.txt, the product, the command line) and
// the order of App.fs.
//
// planDirectLaunch does everything up to the start and starts nothing:
//
//   VersionInfo.txt of the product folder  ->  the machine ID  ->  Frontier's time  ->  a Steam ticket
//   ->  /3.0/user/steam/auth  ->  the ticket is cancelled and Steam shut down  ->  /3.0/user/purchases
//   (the product is the one whose directory is this folder)  ->  /3.0/user/installer, whose version must
//   be the one on disk  ->  the command line.
//
// The result is either a plan to start the game or a fallback with the reason. A fallback is not an
// error: the caller starts the game through Steam instead (docs\design-launcher-2026-11-07.md,
// "Falling back"), and the launcher never patches the game, which is what a version difference is for.
//
// Direct launch is opt-in (launcher.ini, direct_launch=on). The tokens exist only in memory for the length
// of a launch: they are in the plan's command line and nowhere else, and maskSecrets takes them out of
// any text that is shown.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

#include "frontier_api.h"
#include "machine_id.h"
#include "steam_ticket.h"

namespace edvr::installer {

struct DirectEnv {
    HttpClient* http = nullptr;
    std::string apiBase = kFrontierApiBase;
    // Empty means the real one, over the steam_api64.dll at the path it is given.
    std::function<std::unique_ptr<SteamSession>(const std::wstring& dllPath)> makeSteam;
    MachineIdKeys machineKeys;
    ApiClock clock;
    const std::atomic<bool>* cancel = nullptr;
    // One line per step, for the window. Never holds a token. May be empty.
    std::function<void(const std::string&)> progress;
};

struct DirectRequest {
    std::wstring productDir;  // the Products\ folder of the game: GameInstall.dir
    bool vr = true;
};

struct DirectPlan {
    bool start = false;        // true: start the game with exe, args and workDir. False: fall back.
    std::wstring exe;
    std::wstring args;         // after the exe name; holds the tokens
    std::wstring workDir;
    std::string reason;        // when !start: why, as a fragment with no full stop
    std::string detail;        // lines that help a person compare with min-ed-launcher; no token
    std::vector<std::string> secrets;  // every value maskSecrets must remove: the tokens and the machine ID
};

DirectPlan planDirectLaunch(const DirectEnv& env, const DirectRequest& request);

// The band's line for a fallback: the reason, and that the game will use the mode it is set to.
std::string fallbackLine(const std::string& reason);

// Every occurrence of each secret replaced by <masked>.
std::string maskSecrets(const std::string& text, const std::vector<std::string>& secrets);

// The game's arguments, as Product.createArgString builds them without the watchdog:
//   "ServerToken <machineToken> <authToken> <serverArgs>" [/steam] /vr|/novr [gameArgs]
// An offline product has an empty quoted token. `serverArgs` and `gameArgs` are the product's own.
std::string directCommandLine(bool online, const std::string& machineToken, const std::string& authToken,
                              const std::string& serverArgs, bool steamAware, bool vr, const std::string& gameArgs);

// The version compare Product.fs makes (System.Version equality): the same numbers in the same count.
bool sameGameVersion(const std::string& a, const std::string& b);

// The purchase to launch from this product folder: among those naming the folder by directory or by
// SKU, the lowest sortkey whose filter says edo, else the lowest sortkey. Null when none names it.
const Purchase* chooseProduct(const std::vector<Purchase>& items, const std::string& folder);

// Starts a program unelevated with a command line, from a folder, and returns its process handle,
// or null. The caller owns the handle. The real one is realGameStarter; the rig supplies its own.
using GameStarter = std::function<HANDLE(const std::wstring& exe, const std::wstring& args, const std::wstring& workDir)>;
HANDLE startGameProcess(const std::wstring& exe, const std::wstring& args, const std::wstring& workDir);

}  // namespace edvr::installer
