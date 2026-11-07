// launcher.ini: the launcher's own memory, one key=value per line.
//
//   channel            stable or rc
//   current_manifest   SHA-256 of the manifest the payload was last switched to
//   highest_issued     the highest `issued` of any manifest accepted; never lowered
//   mode.<16 hex>      the play mode remembered for one install folder: the first
//                      16 hex characters of the SHA-256 of the folder's path,
//                      lower-cased, with backslashes and no trailing separator
//
// A line this code does not know (another version's key, a comment) is kept as
// it was, in place, so a newer launcher's file survives an older one. Saving is
// atomic: the file is the old bytes or the whole new text.
#pragma once

#include <string>
#include <vector>

namespace edvr::installer {

class LauncherState {
public:
    // Reads `path`. A missing file is an empty state and succeeds; a file that
    // exists and cannot be read fails.
    bool load(const std::wstring& path, std::string* error);

    // Writes the file back through the shared atomic writer. The folder must exist.
    bool save(std::string* error) const;

    const std::wstring& path() const { return path_; }

    std::string get(const std::string& key) const;
    void set(const std::string& key, const std::string& value);

    // "stable" unless the file says "rc".
    std::string channel() const;
    void setChannel(const std::string& channel);

    // A 64-character lower-case hex hash, or empty.
    std::string currentManifest() const;
    void setCurrentManifest(const std::string& sha256);

    long long highestIssued() const;
    // Raises the stored value; a lower or equal one changes nothing.
    void raiseHighestIssued(long long issued);

    static std::string modeKey(const std::wstring& installFolder);
    std::string mode(const std::wstring& installFolder) const;
    void setMode(const std::wstring& installFolder, const std::string& mode);

private:
    std::wstring path_;
    std::vector<std::string> lines_;
};

}  // namespace edvr::installer
