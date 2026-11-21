#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace bttinst {

constexpr const char* kSteamAppId = "1032430";
constexpr const wchar_t* kShippingExe = L"BTVR\\Binaries\\Win64\\BTVR-Win64-Shipping.exe";
constexpr const wchar_t* kWin64 = L"BTVR\\Binaries\\Win64";
constexpr const wchar_t* kModRel = L"ue4ss\\Mods\\BloodTrailTogether";

struct PayloadFile {
  int component = 0;      // 1 = BloodTrail Together, 2 = UE4SS
  std::wstring rel_path;  // relative to BTVR\Binaries\Win64, backslashes
  std::vector<unsigned char> data;
};

struct Payload {
  std::map<std::string, std::string> meta;  // version, supported_builds, ue4ss_version, github_repo
  std::vector<PayloadFile> files;
  bool has_ue4ss() const;
  std::string version() const;
};

// Reads the payload embedded as RCDATA resource 101, or from a file (tests).
bool load_payload_resource(Payload& out, std::string& err);
bool load_payload_file(const std::wstring& path, Payload& out, std::string& err);
bool parse_payload(const unsigned char* d, size_t n, Payload& out, std::string& err);

struct GameInstall {
  std::wstring dir;        // ...\steamapps\common\Blood Trail
  std::string build_id;    // from appmanifest, "" if unknown
  bool from_steam = false;
};

// Steam detection: registry -> libraryfolders.vdf -> appmanifest_1032430.acf
std::vector<std::wstring> steam_libraries();
std::vector<GameInstall> find_game();
bool looks_like_game_dir(const std::wstring& dir);
std::string read_build_id_for(const std::wstring& game_dir);

struct Ue4ssState {
  bool present = false;
  std::wstring proxy;      // e.g. dwmapi.dll
  std::string version;     // from UE4SS.log or file version, may be ""
  bool proxy_conflict = false;  // a proxy dll name we would use exists but is not UE4SS
};
Ue4ssState detect_ue4ss(const std::wstring& game_dir, const Payload* p);

struct InstallStatus {
  bool installed = false;
  std::string version;
  bool ue4ss_by_us = false;
};
InstallStatus read_install_status(const std::wstring& game_dir);

bool game_running();

using Progress = std::function<void(int percent, const std::string& line)>;

struct Result {
  bool ok = false;
  std::string message;
};

// Installs or repairs. Never touches executables, keeps other mods and user settings, backs up
// a previous BloodTrail Together version before replacing it.
Result install(const std::wstring& game_dir, const Payload& p, const Progress& progress);
// Removes BloodTrail Together; removes UE4SS only if we installed it and remove_ue4ss is set.
Result uninstall(const std::wstring& game_dir, bool remove_ue4ss, const Progress& progress);

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);

}  // namespace bttinst
