// Ported from Product.fs and App.fs of min-ed-launcher, commit 139e4ff7ab9027925648fb27767cb87acdcf290a.
// MIT licence, "Copyright (c) 2017 Chris": see third_party\min_ed_launcher\LICENSE.
#include "direct_launch.h"

#include <cctype>
#include <cstdlib>

#include "detect.h"
#include "launch_route.h"
#include "update_json.h"
#include "update_util.h"

namespace edvr::installer {
namespace {

struct VersionInfo {
    std::string version;
    std::string executable;
    bool steamAware = true;
    bool offline = false;
};

// Product.readVersionInfo: "Version", "executable" and "name" must be there; steamaware defaults to
// true and offline to false.
bool readVersionInfo(const std::wstring& productDir, VersionInfo* out) {
    std::string bytes;
    if (!readFileBytes(joinPath(productDir, L"VersionInfo.txt"), &bytes, 1u << 20, nullptr)) return false;
    if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
    JsonValue json;
    if (!parseJson(bytes, &json, nullptr) || !json.isObject()) return false;
    const JsonValue* version = json.findNoCase("Version");
    const JsonValue* exe = json.findNoCase("executable");
    const JsonValue* name = json.findNoCase("name");
    if (!version || !version->isString() || !exe || !exe->isString() || !name || !name->isString()) return false;
    out->version = version->text;
    out->executable = exe->text;
    // This file writes booleans as strings as well: its useWatchDog64 is "true".
    const JsonValue* aware = json.findNoCase("steamaware");
    if (aware && aware->type == JsonValue::Type::Bool) out->steamAware = aware->boolean;
    else if (aware && aware->isString()) out->steamAware = aware->text != "false";
    const JsonValue* offline = json.findNoCase("offline");
    out->offline = offline && offline->type == JsonValue::Type::Bool && offline->boolean;
    return true;
}

// "4.2.3.0" as numbers. System.Version takes two to four of them.
bool parseVersion(const std::string& text, std::vector<long long>* parts) {
    parts->clear();
    size_t start = 0;
    for (;;) {
        const size_t end = text.find('.', start);
        const std::string piece = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (piece.empty() || piece.size() > 9) return false;
        for (char c : piece) {
            if (c < '0' || c > '9') return false;
        }
        parts->push_back(std::atol(piece.c_str()));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts->size() >= 2 && parts->size() <= 4;
}

std::string lastFolder(const std::wstring& path) {
    std::wstring trimmed = path;
    while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    const size_t slash = trimmed.find_last_of(L"\\/");
    return narrowUtf8(slash == std::wstring::npos ? trimmed : trimmed.substr(slash + 1));
}

bool sameText(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

DirectPlan fallback(DirectPlan plan, const std::string& reason) {
    plan.start = false;
    plan.reason = reason;
    return plan;
}

// One reason for each way a call can end, the words of the design's table.
std::string reasonFor(const ApiResult& result) {
    if (result.outcome == ApiOutcome::RegistrationRequired || result.outcome == ApiOutcome::LinkAvailable)
        return "Your Steam and Frontier accounts need linking (HTTP " + std::to_string(result.httpStatus) + ")";
    return result.message;
}

// An ownership page is reported as intermittent: the call is made once more, and only a second page is an answer.
ApiResult callWithRetry(const DirectEnv& env, const std::function<ApiResult()>& call) {
    ApiResult result = call();
    if (result.outcome == ApiOutcome::OwnershipPage) {
        if (env.progress) env.progress("Frontier could not confirm ownership. Asking once more...");
        result = call();
    }
    return result;
}

}  // namespace

const Purchase* chooseProduct(const std::vector<Purchase>& items, const std::string& folder) {
    // Odyssey and live Horizons share one folder and differ by filter, so the folder alone does not
    // decide. As App.fs does with /edo: lowest sortkey first, and the first whose filter says edo wins.
    const Purchase* first = nullptr;
    const Purchase* odyssey = nullptr;
    for (const Purchase& p : items) {
        // fixDirectoryName: a Steam install's folder can be named by the SKU.
        if (p.sku.empty() || (!sameText(p.directory, folder) && !sameText(p.sku, folder))) continue;
        if (!first || p.sortKey < first->sortKey) first = &p;
        bool saysEdo = false;
        size_t start = 0;
        while (start <= p.filter.size()) {
            const size_t comma = p.filter.find(',', start);
            const std::string piece = p.filter.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (sameText(piece, "edo")) saysEdo = true;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (saysEdo && (!odyssey || p.sortKey < odyssey->sortKey)) odyssey = &p;
    }
    return odyssey ? odyssey : first;
}

bool sameGameVersion(const std::string& a, const std::string& b) {
    std::vector<long long> x, y;
    return parseVersion(a, &x) && parseVersion(b, &y) && x == y;
}

std::string directCommandLine(bool online, const std::string& machineToken, const std::string& authToken,
                              const std::string& serverArgs, bool steamAware, bool vr, const std::string& gameArgs) {
    std::string options;
    auto add = [&](const std::string& piece) {
        if (!options.empty()) options += ' ';
        options += piece;
    };
    if (steamAware) add("/steam");
    add(vr ? "/vr" : "/novr");
    if (!gameArgs.empty()) add(gameArgs);
    const std::string serverToken = online ? "ServerToken " + machineToken + " " + authToken + " " + serverArgs : std::string();
    return "\"" + serverToken + "\" " + options;
}

std::string fallbackLine(const std::string& reason) {
    return "Direct launch did not work: " + reason + ". Starting through Steam instead; Elite will use the mode it is set to.";
}

std::string maskSecrets(const std::string& text, const std::vector<std::string>& secrets) {
    std::string out = text;
    for (const std::string& secret : secrets) {
        if (secret.empty()) continue;
        size_t at = 0;
        while ((at = out.find(secret, at)) != std::string::npos) {
            out.replace(at, secret.size(), "<masked>");
            at += 8;
        }
    }
    return out;
}

DirectPlan planDirectLaunch(const DirectEnv& env, const DirectRequest& request) {
    DirectPlan plan;
    auto say = [&](const std::string& line) {
        if (env.progress) env.progress(line);
    };
    auto stopped = [&]() { return env.cancel && env.cancel->load(); };

    // 1. The product folder.
    say("Reading the game's version...");
    VersionInfo info;
    if (!readVersionInfo(request.productDir, &info)) return fallback(plan, "Elite's VersionInfo.txt could not be read from its folder");
    std::vector<long long> localParts;
    if (!parseVersion(info.version, &localParts)) return fallback(plan, "Elite's VersionInfo.txt has a version the launcher cannot read");
    const std::wstring exe = joinPath(request.productDir, widenUtf8(info.executable));
    if (!fileExists(exe)) return fallback(plan, "Elite's program was not found in its folder");

    // Steam's library sits beside EDLaunch.exe, which is at the top of the install.
    const std::wstring edlaunch = findEdLaunch(request.productDir);
    const size_t slash = edlaunch.find_last_of(L"\\/");
    const std::wstring dll = edlaunch.empty() ? std::wstring() : joinPath(edlaunch.substr(0, slash), L"steam_api64.dll");
    if (dll.empty() || !fileExists(dll)) return fallback(plan, "steam_api64.dll was not found beside Elite's launcher");

    // 2. The machine ID.
    std::string machineId, error;
    if (!readMachineId(env.machineKeys, &machineId, &error)) return fallback(plan, error);
    plan.secrets.push_back(machineId);

    // 3. Frontier's clock, then 4. a ticket from Steam: the order App.fs takes them in.
    if (stopped()) return fallback(plan, "The sign-in was stopped");
    FrontierApi api(*env.http, env.apiBase, env.cancel, env.clock);
    say("Asking Frontier for the time...");
    api.syncTime();

    say("Asking Steam for a sign-in ticket...");
    std::unique_ptr<SteamSession> steam = env.makeSteam ? env.makeSteam(dll) : makeSteamSession(dll);
    std::string ticket;
    if (!steam || !steam->getTicket(&ticket, &error)) {
        if (steam) steam->release();
        return fallback(plan, steam ? error : "Steam could not be reached");
    }
    plan.secrets.push_back(ticket);

    // 5. Sign in. The ticket is cancelled and Steam shut down as soon as Frontier has had it, which is
    // before anything else is asked and so before the game is started, whatever the answer was.
    say("Signing in to Frontier with your Steam account...");
    FrontierSession session;
    const ApiResult signedIn = callWithRetry(env, [&]() { return api.steamAuth(machineId, ticket, &session); });
    steam->release();
    steam.reset();
    plan.secrets.push_back(session.authToken);
    plan.secrets.push_back(session.machineToken);
    if (signedIn.outcome != ApiOutcome::Ok) return fallback(plan, reasonFor(signedIn));

    // 6. The product among the purchases.
    say("Checking what the account owns...");
    PurchaseList purchases;
    const ApiResult bought = callWithRetry(env, [&]() { return api.purchases(session, &purchases); });
    if (bought.outcome != ApiOutcome::Ok) return fallback(plan, reasonFor(bought));
    const Purchase* product = chooseProduct(purchases.items, lastFolder(request.productDir));
    plan.detail = "Purchases: " + std::to_string(purchases.items.size()) + ", fields: " + purchases.shape + "\r\n";
    if (!product) return fallback(plan, "Elite is not among the products Frontier lists for this account");

    // 7. The version on disk against Frontier's.
    say("Checking the game's version...");
    std::string remoteVersion;
    const ApiResult installer =
        callWithRetry(env, [&]() { return api.installerVersion(session, machineId, product->sku, &remoteVersion); });
    if (installer.outcome != ApiOutcome::Ok) return fallback(plan, reasonFor(installer));
    plan.detail += "Version on disk: " + info.version + ", Frontier's: " + remoteVersion + "\r\n";
    std::vector<long long> remoteParts;
    if (!parseVersion(remoteVersion, &remoteParts)) return fallback(plan, "Frontier named a game version the launcher cannot read");
    if (!sameGameVersion(info.version, remoteVersion))
        return fallback(plan, "The game needs patching (installed " + info.version + ", current " + remoteVersion + ")");

    // 8. The command line.
    std::string serverArgs;
    if (product->testApi) serverArgs = "/Test";
    if (!product->serverArgs.empty()) serverArgs += (serverArgs.empty() ? "" : " ") + product->serverArgs;
    plan.exe = exe;
    plan.workDir = request.productDir;
    plan.args = widenUtf8(directCommandLine(!info.offline, session.machineToken, session.authToken, serverArgs, info.steamAware,
                                            request.vr, product->gameArgs));
    plan.start = true;
    return plan;
}

HANDLE startGameProcess(const std::wstring& exe, const std::wstring& args, const std::wstring& workDir) {
    std::wstring line = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), &line[0], nullptr, nullptr, FALSE, 0, nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi))
        return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

}  // namespace edvr::installer
