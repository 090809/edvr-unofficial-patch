#pragma once

// The public keys the launcher trusts for release manifests. A manifest is
// accepted when its signature verifies against any key listed here.
//
// Each key is a P-256 public key written as 128 hex characters: the x
// coordinate (64 characters) followed by the y coordinate (64 characters).
// `python tools\sign_manifest.py generate` makes a pair and prints both halves
// once. The public half goes in this list; the private half goes in the
// maintainer's password manager and never into a repository, a log or a
// session. The list holds the main key first and a recovery key second, so that
// losing one does not strand installed launchers.
//
// No keys are set yet. tools\sign_manifest.py reads this file and refuses to
// sign while it holds none.

inline constexpr const char* kUpdatePublicKeys[] = {
    // main key: 128 hex chars, x then y
    // recovery key: 128 hex chars, x then y
    nullptr,
};
