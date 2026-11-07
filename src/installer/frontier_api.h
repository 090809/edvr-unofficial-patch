// The four calls a direct launch makes to Frontier's servers, over the launcher's HttpClient.
// A port of the Steam path of Api.fs from min-ed-launcher (https://github.com/rfvgyhn/min-ed-launcher,
// commit 139e4ff7ab9027925648fb27767cb87acdcf290a, MIT licence, "Copyright (c) 2017 Chris"; the notice
// is third_party\min_ed_launcher\LICENSE). The parameter names and their order are the port's, because
// a flight compares the two:
//
//   GET /1.1/server/time        no sign-in. unixTimestamp is the server's clock.
//   GET /3.0/user/steam/auth    steamTicket, machineId, fTime. No Authorization header.
//   GET /3.0/user/purchases     fTime. Authorization: bearer <authToken>.
//   GET /3.0/user/installer     machineToken, authToken, machineId, sku, os=win, fTime. The same header.
//
// fTime is the server's time from the first call plus the seconds since, so a clock that is wrong on
// this machine does not matter. When the time call fails the local UTC clock stands in, as there.
//
// Nothing here follows a redirect: a 302 or 307 is an answer, and a bearer token must never be sent
// anywhere but the host it was issued by. Replies are capped at 4 MiB. The user agent is whatever the
// HttpClient says: the product's WinHttpClient sends "ed-up-launcher/<version>", never min-ed-launcher's
// and never EDLaunch's.
//
// The URLs carry tokens in their query strings, so no URL is put in a message or an error: a result's
// `message` is a sentence about what happened and holds no value that came from or went to the server.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "update_http.h"

namespace edvr::installer {

enum class ApiOutcome {
    Ok,
    Denied,                // 400, 401 or 403
    Unavailable,           // no answer, or a 5xx
    OwnershipPage,         // 200 with an HTML page: Frontier could not confirm ownership (reported as intermittent)
    RegistrationRequired,  // 302
    LinkAvailable,         // 307
    Unexpected,            // any other status, or a reply that is not what the call needs
};

struct ApiResult {
    ApiOutcome outcome = ApiOutcome::Unexpected;
    int httpStatus = 0;
    std::string message;  // a sentence, with no token or address in it
};

struct FrontierSession {
    std::string authToken;
    std::string machineToken;
};

struct Purchase {
    std::string sku;
    std::string name;
    std::string directory;  // the Products\ folder name
    std::string filter;
    std::string gameArgs;
    std::string serverArgs;
    bool testApi = false;
    long long sortKey = 0;
};

struct PurchaseList {
    std::vector<Purchase> items;
    // The field names of the first purchase, comma separated. They are not secret, and say what the
    // server calls things when the port's guess at them was wrong.
    std::string shape;
};

struct ApiClock {
    std::function<long long()> unixNow;           // seconds since 1970 UTC; empty means the system clock
    std::function<unsigned long long()> tickMs;   // a monotonic millisecond count; empty means GetTickCount64
};

inline constexpr unsigned long long kFrontierMaxBytes = 4ull << 20;
inline constexpr const char* kFrontierApiBase = "https://api.zaonce.net";

class FrontierApi {
public:
    // `http` must outlive this. `baseUrl` has no trailing slash.
    FrontierApi(HttpClient& http, std::string baseUrl, const std::atomic<bool>* cancel = nullptr,
                ApiClock clock = ApiClock());

    // Asks for the server's time. True when the server answered, false when the local clock is used.
    bool syncTime();
    // The running time, in whole seconds, as the text the other calls send.
    std::string fTime() const;

    ApiResult steamAuth(const std::string& machineId, const std::string& steamTicketHex, FrontierSession* session);
    ApiResult purchases(const FrontierSession& session, PurchaseList* out);
    ApiResult installerVersion(const FrontierSession& session, const std::string& machineId, const std::string& sku,
                               std::string* version);

private:
    HttpResponse send(const std::string& path, const std::vector<std::pair<std::string, std::string>>& query,
                      const std::string& bearer);

    HttpClient& http_;
    std::string base_;
    const std::atomic<bool>* cancel_;
    ApiClock clock_;
    long long baseSeconds_ = 0;
    unsigned long long baseTick_ = 0;
};

}  // namespace edvr::installer
