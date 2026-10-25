#pragma once

//DH1KLM The folder swap AetherSDRUpdater.exe performs after AetherSDR exits (RFC
//DH1KLM #5304 Phase 3). Plain C++17 with no Qt and no project library: the helper
//DH1KLM runs while the install it came from is being renamed, so it may depend on
//DH1KLM nothing in that folder. Everything that touches processes is injected
//DH1KLM (Ops), so the whole state machine is exercised by update_swap_test on any
//DH1KLM platform.
//DH1KLM
//DH1KLM The swap, beside the install, on its own volume:
//DH1KLM
//DH1KLM   <install>.new       the staged release, renamed (or copied, across
//DH1KLM                       volumes) next to the install
//DH1KLM   <install>           renamed to <install>.previous, then .new takes its name
//DH1KLM   <install>.previous  kept: the one version a rollback restores
//DH1KLM   <install>.failed    a release that was rolled back (one kept, for logs)
//DH1KLM
//DH1KLM Windows has no atomic directory swap: between the install's rename and
//DH1KLM .new's, nothing runnable is at <install> for a moment. A journal in the
//DH1KLM state folder records each step before it is taken, so recover() can put
//DH1KLM the install back if the helper dies there. Because a missing install
//DH1KLM cannot start the app that would start the helper, the helper also arms a
//DH1KLM repair (on Windows a RunOnce entry: repair() at the next logon) before
//DH1KLM the first rename, and disarms it once nothing is owed.
//DH1KLM
//DH1KLM After the swap the new AetherSDR runs a self-test (--update-selftest
//DH1KLM <marker>): it starts Qt, loads its plugins and libraries, writes the
//DH1KLM marker and exits, with no window, no settings writes and no radio. That
//DH1KLM catches what a swap can break (a missing or bad DLL or plugin) and is safe
//DH1KLM after a normal quit, when opening the app again would be wrong. If the
//DH1KLM self-test exits without the marker, or does not finish in time, the
//DH1KLM previous version is restored. Only Restart to update then starts the app.
//DH1KLM
//DH1KLM The outcome is written to <stateDir>/swap-result.txt ("<outcome>
//DH1KLM <version>"), which the app reads at its next start: a rolled-back release
//DH1KLM is remembered there, so it is never staged again in a loop.

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace AetherSDR::updater {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Duration = std::chrono::milliseconds;

//DH1KLM The helper's command-line contract. The app that launches the helper is
//DH1KLM the running (old) version, so the version is checked first and anything
//DH1KLM else is refused rather than guessed at.
constexpr int kProtocol = 1;

struct Plan {
    fs::path install;    //DH1KLM the folder AetherSDR runs from
    fs::path staged;     //DH1KLM the checked new release (UpdateStager's output)
    fs::path stateDir;   //DH1KLM journal and health marker; outside the install
    fs::path exeName;    //DH1KLM "AetherSDR.exe", relative to install
    std::string version; //DH1KLM the release being installed, "26.10.4"
    bool relaunch = false; //DH1KLM Restart to update: start the app when done
    Duration exitTimeout = std::chrono::minutes(5);
    Duration renameTimeout = std::chrono::seconds(30);
    Duration healthTimeout = std::chrono::seconds(60);
};

enum class Outcome {
    Updated,           //DH1KLM the new version is in place and passed its self-test
    RolledBack,        //DH1KLM it failed the self-test; the previous version is back
    OldStillRunning,   //DH1KLM AetherSDR never exited; nothing was touched
    NotPossible,       //DH1KLM a check before any change failed; nothing was touched
    SwapUndone,        //DH1KLM a rename failed mid-swap and was undone
    Broken,            //DH1KLM a rollback rename failed: no runnable tree; the user must act
};

//DH1KLM The names swap-result.txt uses (UpdateInstaller reads them back).
inline const char* outcomeName(Outcome o)
{
    switch (o) {
    case Outcome::Updated:         return "Updated";
    case Outcome::RolledBack:      return "RolledBack";
    case Outcome::OldStillRunning: return "OldStillRunning";
    case Outcome::NotPossible:     return "NotPossible";
    case Outcome::SwapUndone:      return "SwapUndone";
    case Outcome::Broken:          return "Broken";
    }
    return "Unknown";
}

//DH1KLM A started process, opaque to the core.
struct Process {
    std::shared_ptr<void> handle;
};

struct Ops {
    //DH1KLM Whether the old AetherSDR has exited within timeout.
    std::function<bool(Duration timeout)> waitForOldExit;
    //DH1KLM Starts exe with args (native strings, so a path keeps every
    //DH1KLM character on Windows); an empty handle when it could not.
    std::function<Process(const fs::path& exe, const std::vector<fs::path>& args)> launch;
    //DH1KLM Whether p has exited (non-blocking).
    std::function<bool(const Process& p)> hasExited;
    std::function<void(const Process& p)> terminate;
    std::function<void(Duration)> sleep;
    std::function<Clock::time_point()> now;
    //DH1KLM A line for the helper's log.
    std::function<void(const std::string&)> log;
    //DH1KLM Renames a folder; empty means std::filesystem::rename. Tests inject
    //DH1KLM the failures a real system produces (a locked folder, another volume).
    std::function<void(const fs::path& from, const fs::path& to, std::error_code& ec)> rename;
    //DH1KLM Writes the journal; empty means a write-then-rename to the real file.
    //DH1KLM Tests inject a failed write at a chosen step.
    std::function<bool(const fs::path& path, const std::string& text)> writeJournal;
    //DH1KLM Arms a repair that runs repair() even if nothing starts the app (the
    //DH1KLM next logon); true once it is in place. Disarmed when no repair is
    //DH1KLM owed. Both empty: no repair is armed (tests that don't look).
    std::function<bool()> armRepair;
    std::function<void()> disarmRepair;
    //DH1KLM A file's identity, kept across renames and new for a file written
    //DH1KLM again (on Windows the volume serial and NTFS file ID); empty when
    //DH1KLM unknown. The journal records the new release's AetherSDR.exe, so a
    //DH1KLM repair never rolls back a tree it did not leave (a reinstall).
    //DH1KLM Empty function: no identity check.
    std::function<std::string(const fs::path&)> fileId;
};

struct Paths {
    fs::path next;      //DH1KLM <install>.new
    fs::path previous;  //DH1KLM <install>.previous
    fs::path failed;    //DH1KLM <install>.failed
    fs::path journal;   //DH1KLM <stateDir>/swap-journal.txt
    fs::path marker;    //DH1KLM <stateDir>/selftest-<version>.txt
    fs::path result;    //DH1KLM <stateDir>/swap-result.txt
};

Paths pathsFor(const Plan& plan);

//DH1KLM A path as UTF-8, whatever the code page, for the log and the plan file.
std::string utf8(const fs::path& p);

//DH1KLM Puts the install back if a previous helper died mid-swap (the journal
//DH1KLM says so). Returns false only when it found a broken state it could not
//DH1KLM repair. Safe to call when there is no journal. moved (if given) says
//DH1KLM whether any folder was renamed.
bool recover(const Plan& plan, const Ops& ops, bool* moved = nullptr);

//DH1KLM The whole update: wait for the old process, check, swap, start, watch,
//DH1KLM and roll back when the new version does not come up.
Outcome run(const Plan& plan, const Ops& ops);

//DH1KLM The armed repair: recover() alone, with no swap (plan.staged is unused).
//DH1KLM It re-arms itself first, since Windows deletes a RunOnce value before
//DH1KLM running it and an interrupted repair must run again. While AetherSDR runs
//DH1KLM from the install (ops.waitForOldExit) nothing is renamed and it waits for
//DH1KLM the next logon. A repair that moved the install back records SwapUndone,
//DH1KLM never RolledBack, since the release was never tested. False while the
//DH1KLM install is still broken: the repair stays armed and the journal kept.
bool repair(const Plan& plan, const Ops& ops);

} //DH1KLM namespace AetherSDR::updater
