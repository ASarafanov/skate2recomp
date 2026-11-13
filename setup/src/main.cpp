// SPDX-License-Identifier: MIT
// Skate 2 PC setup and launcher. One exe, two roles:
//  - with the payload appended (Skate2-PC-Setup.exe): the install window;
//  - without it, inside an install (Launch Skate 2.exe): starts the game
//    right away; Shift or --options opens the options first.
#include "disc.h"
#include "install.h"
#include "launch.h"
#include "resource.h"
#include "util.h"
#include "version.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cwchar>
#include <memory>
#include <thread>

using namespace skate2;

namespace {

constexpr UINT kMsgInstallDone = WM_APP + 1;
constexpr UINT_PTR kTimerProgress = 1;
constexpr UINT_PTR kTimerCheckIso = 2;
constexpr uint64_t kSpareBytes = 256ull << 20;
constexpr int kExitPlay = 100;

struct Options {
  bool setup = false;    // show the setup even when installed
  bool options = false;  // show the launch options before starting
  bool play = false;     // setup exe: start the installed game
  bool quiet = false;    // install without a window (needs --iso and --dir)
  std::wstring iso, dir;
  std::optional<int> render_scale;
  std::optional<std::wstring> frame_rate;
  bool windowed = false;
  std::vector<std::wstring> passthrough;  // --flag=value for skate2.exe
};

Options ParseOptions() {
  Options options;
  int count = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
  for (int i = 1; argv && i < count; ++i) {
    std::wstring arg = argv[i];
    std::wstring value;
    const size_t equals = arg.find(L'=');
    std::wstring name = arg.substr(0, equals);
    const bool has_value = equals != std::wstring::npos;
    if (has_value) value = arg.substr(equals + 1);
    auto next = [&]() -> std::wstring {
      if (has_value) return value;
      return i + 1 < count ? std::wstring(argv[++i]) : std::wstring();
    };
    if (name == L"--setup") options.setup = true;
    else if (name == L"--options") options.options = true;
    else if (name == L"--play") options.play = true;
    else if (name == L"--quiet") options.quiet = true;
    else if (name == L"--iso") options.iso = next();
    else if (name == L"--dir") options.dir = next();
    else if (name == L"--windowed") options.windowed = true;
    else if (name == L"--render-scale") {
      const int scale = _wtoi(next().c_str());
      if (scale >= 1 && scale <= 4) options.render_scale = scale;
    } else if (name == L"--frame-rate") {
      options.frame_rate = next();
    } else if (arg.rfind(L"--", 0) == 0 && has_value) {
      options.passthrough.push_back(arg);
    }
  }
  if (argv) LocalFree(argv);
  return options;
}

std::wstring GetText(HWND dialog, int id) {
  HWND control = GetDlgItem(dialog, id);
  std::wstring text(size_t(GetWindowTextLengthW(control)) + 1, L'\0');
  GetWindowTextW(control, text.data(), int(text.size()));
  text.resize(wcslen(text.c_str()));
  // Paths pasted from Explorer often come with quotes.
  while (!text.empty() && (text.front() == L'"' || text.front() == L' ')) text.erase(0, 1);
  while (!text.empty() && (text.back() == L'"' || text.back() == L' ')) text.pop_back();
  return text;
}

std::wstring DefaultInstallDir() {
  if (IsWine()) {
    const std::wstring home = WineHome();
    if (!home.empty()) return Join(home, L"Games\\Skate 2");
  }
  return L"C:\\Games\\Skate 2";
}

// IFileOpenDialog for a disc image or a folder. Empty when cancelled.
std::wstring PickPath(HWND owner, bool folder, const std::wstring& start) {
  std::wstring result;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) {
    return result;
  }
  FILEOPENDIALOGOPTIONS flags = 0;
  dialog->GetOptions(&flags);
  if (folder) {
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose where to install Skate 2");
  } else {
    const COMDLG_FILTERSPEC filters[] = {
        {L"Disc image or extracted default.xex (*.iso; default.xex)", L"*.iso;default.xex"},
        {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, filters);
    dialog->SetOptions(flags | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Select your Skate 2 disc image, or default.xex of the extracted game");
  }
  std::wstring start_dir = start;
  while (!start_dir.empty() && !IsDir(start_dir)) {
    const std::wstring parent = DirName(start_dir);
    if (parent == start_dir) break;
    start_dir = parent;
  }
  IShellItem* item = nullptr;
  if (!start_dir.empty() && IsDir(start_dir) &&
      SUCCEEDED(SHCreateItemFromParsingName(start_dir.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
    dialog->SetFolder(item);
    item->Release();
  }
  if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item))) {
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
      result = path;
      CoTaskMemFree(path);
    }
    item->Release();
  }
  dialog->Release();
  return result;
}

// --- options dialog --------------------------------------------------------

struct OptionsState {
  LaunchSettings settings;
  bool play_button = true;
};

INT_PTR CALLBACK OptionsProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<OptionsState*>(GetWindowLongPtrW(dialog, DWLP_USER));
  switch (message) {
    case WM_INITDIALOG: {
      state = reinterpret_cast<OptionsState*>(lparam);
      SetWindowLongPtrW(dialog, DWLP_USER, LONG_PTR(state));
      const LaunchSettings& s = state->settings;
      HWND scale = GetDlgItem(dialog, IDC_SCALE);
      for (const wchar_t* text : {L"1x - 1152x640, fastest", L"2x - sharper (default on PC)",
                                  L"3x - experimental", L"4x - experimental"}) {
        SendMessageW(scale, CB_ADDSTRING, 0, LPARAM(text));
      }
      SendMessageW(scale, CB_SETCURSEL, WPARAM(s.render_scale - 1), 0);
      HWND fps = GetDlgItem(dialog, IDC_FPS);
      for (size_t i = 0; i < kFrameRateCount; ++i) {
        std::wstring text = kFrameRates[i];
        if (text == L"original") text = L"Original (as on Xbox 360)";
        else if (text == L"unlimited") text = L"Unlimited";
        else text += L" FPS";
        SendMessageW(fps, CB_ADDSTRING, 0, LPARAM(text.c_str()));
        if (s.frame_rate == kFrameRates[i]) SendMessageW(fps, CB_SETCURSEL, i, 0);
      }
      HWND path = GetDlgItem(dialog, IDC_PATH);
      SendMessageW(path, CB_ADDSTRING, 0, LPARAM(L"ROV (recommended)"));
      SendMessageW(path, CB_ADDSTRING, 0, LPARAM(L"RTV (if the picture is broken)"));
      SendMessageW(path, CB_SETCURSEL, s.render_path == L"rtv" ? 1 : 0, 0);
      CheckDlgButton(dialog, IDC_WINDOWED, s.windowed ? BST_CHECKED : BST_UNCHECKED);
      CheckDlgButton(dialog, IDC_ALWAYS, s.show_options ? BST_CHECKED : BST_UNCHECKED);
      SetDlgItemTextW(dialog, IDC_OPTIONS_NOTE,
                      L"Hold Shift while starting Launch Skate 2.exe to open this window. More "
                      L"PC settings are in the game: press F4.");
      SetDlgItemTextW(dialog, IDOK, state->play_button ? L"Play" : L"Save");
      return TRUE;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == IDOK) {
        LaunchSettings& s = state->settings;
        s.render_scale = int(SendDlgItemMessageW(dialog, IDC_SCALE, CB_GETCURSEL, 0, 0)) + 1;
        const LRESULT fps = SendDlgItemMessageW(dialog, IDC_FPS, CB_GETCURSEL, 0, 0);
        if (fps >= 0 && size_t(fps) < kFrameRateCount) s.frame_rate = kFrameRates[fps];
        s.render_path =
            SendDlgItemMessageW(dialog, IDC_PATH, CB_GETCURSEL, 0, 0) == 1 ? L"rtv" : L"rov";
        s.windowed = IsDlgButtonChecked(dialog, IDC_WINDOWED) == BST_CHECKED;
        s.show_options = IsDlgButtonChecked(dialog, IDC_ALWAYS) == BST_CHECKED;
        EndDialog(dialog, IDOK);
        return TRUE;
      }
      if (LOWORD(wparam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
      }
      break;
  }
  return FALSE;
}

// Shows the options for the install in `dir` and saves them. True on OK.
bool EditOptions(HWND owner, const std::wstring& dir, bool play_button) {
  OptionsState state{LoadSettings(dir), play_button};
  if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_OPTIONS), owner,
                      OptionsProc, LPARAM(&state)) != IDOK) {
    return false;
  }
  if (!SaveSettings(dir, state.settings)) {
    MessageBoxW(owner, L"Could not save the options.", L"Skate 2", MB_ICONWARNING);
  }
  return true;
}

// --- setup dialog ----------------------------------------------------------

struct SetupState {
  const Options* options = nullptr;
  const Payload* payload = nullptr;  // null: portable, game files only
  std::wstring fixed_dir;            // portable: the folder of this exe
  std::optional<Disc> disc;
  std::wstring checked_iso;
  std::wstring iso_error;
  bool working = false;
  bool finished = false;
  std::unique_ptr<InstallProgress> progress;
  std::thread worker;
  InstallResult result = InstallResult::kDone;
  std::wstring error;
  std::wstring play_dir;  // set when the dialog ends with kExitPlay
};

std::wstring TargetOf(HWND dialog, const SetupState& state) {
  if (!state.fixed_dir.empty()) return state.fixed_dir;
  const std::wstring dir = GetText(dialog, IDC_DIR);
  return dir.empty() ? std::wstring() : InstallTarget(dir);
}

void CheckIso(HWND dialog, SetupState& state) {
  const std::wstring iso = GetText(dialog, IDC_ISO);
  if (iso == state.checked_iso) return;
  state.checked_iso = iso;
  state.disc.reset();
  state.iso_error.clear();
  if (iso.empty()) return;
  if (!IsFile(iso) && !IsDir(iso)) {
    state.iso_error = L"File not found.";
    return;
  }
  HCURSOR cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  Disc disc;
  state.iso_error = CheckDisc(iso, disc);
  SetCursor(cursor);
  if (state.iso_error.empty()) state.disc = std::move(disc);
}

void UpdateUi(HWND dialog, SetupState& state) {
  const std::wstring target = TargetOf(dialog, state);
  const bool has_game = !target.empty() && HasGameFiles(target);
  const bool installed = !target.empty() && IsInstalled(target);

  // Disc line.
  std::wstring iso_text;
  if (state.disc) {
    iso_text = std::wstring(state.disc->folder ? L"Extracted Skate 2 game folder"
                                               : L"Skate 2 disc image") +
               L", supported release (media ID 6ADB5821). " + SizeText(state.disc->total_bytes) +
               L" of game files.";
  } else if (!state.iso_error.empty()) {
    iso_text = state.iso_error;
  } else if (has_game) {
    iso_text = L"Not needed: the game files are already installed. Select it only to copy them "
               L"again.";
  } else {
    iso_text = L"Select the .iso image of your Xbox 360 Skate 2 disc, or the extracted game "
               L"folder (with default.xex and data). Drop either onto this window.";
  }
  SetDlgItemTextW(dialog, IDC_ISO_STATUS, iso_text.c_str());

  // Folder line.
  bool folder_ok = !target.empty();
  std::wstring dir_text;
  if (target.empty()) {
    dir_text = L"Choose an install folder.";
  } else if (!IsAsciiPath(target)) {
    folder_ok = false;
    dir_text = L"Choose a folder whose path has only English letters, digits and common "
               L"symbols: the game cannot open other paths.";
  } else {
    dir_text = L"Installs into " + target + L".";
    uint64_t needed = kSpareBytes;
    if (state.payload) needed += state.payload->total_bytes;
    if (state.disc) needed += state.disc->total_bytes;
    if (const auto free = FreeSpace(target)) {
      dir_text += L" Free: " + SizeText(*free) + L", needed: " + SizeText(needed) + L".";
      if (*free < needed) {
        folder_ok = false;
        dir_text += L" Not enough space.";
      }
    }
    if (IsFile(Join(target, kMarkerName)) && !state.working) {
      dir_text += L" An unfinished install is here; it will be resumed.";
    } else if (installed) {
      dir_text += L" Skate 2 is installed here.";
    }
  }
  SetDlgItemTextW(dialog, IDC_DIR_STATUS, dir_text.c_str());

  const bool can_install = !state.working && folder_ok &&
                           (state.disc || (has_game && state.payload));
  EnableWindow(GetDlgItem(dialog, IDC_INSTALL), can_install);
  SetDlgItemTextW(dialog, IDC_INSTALL, !state.disc && has_game ? L"Update" : L"Install");
  EnableWindow(GetDlgItem(dialog, IDC_PLAY), !state.working && installed);
  EnableWindow(GetDlgItem(dialog, IDC_OPTIONS), !state.working && installed);
  for (int id : {IDC_ISO, IDC_ISO_BROWSE, IDC_DESKTOP, IDC_STARTMENU}) {
    EnableWindow(GetDlgItem(dialog, id), !state.working);
  }
  for (int id : {IDC_DIR, IDC_DIR_BROWSE}) {
    EnableWindow(GetDlgItem(dialog, id), !state.working && state.fixed_dir.empty());
  }
  SetDlgItemTextW(dialog, IDCANCEL, state.working ? L"Cancel" : L"Close");
  // Keep Enter on the useful button.
  const int default_id = installed && !state.disc ? IDC_PLAY : IDC_INSTALL;
  SendMessageW(dialog, DM_SETDEFID, default_id, 0);
}

void ShowProgress(HWND dialog, SetupState& state) {
  const uint64_t total = state.progress->total_bytes.load();
  const uint64_t done = state.progress->done_bytes.load();
  SendDlgItemMessageW(dialog, IDC_PROGRESS, PBM_SETPOS,
                      total ? WPARAM(done * 1000 / total) : 0, 0);
  std::wstring current;
  {
    std::lock_guard lock(state.progress->mutex);
    current = state.progress->current;
  }
  std::wstring text = L"Installing: " + SizeText(done) + L" of " + SizeText(total) + L"\n" +
                      current;
  if (state.progress->cancel.load()) text = L"Cancelling...";
  SetDlgItemTextW(dialog, IDC_STATUS, text.c_str());
}

void StartInstall(HWND dialog, SetupState& state) {
  const std::wstring target = TargetOf(dialog, state);
  if (!IsAsciiPath(target)) return;
  if (state.worker.joinable()) state.worker.join();
  MakeDirs(target);
  OpenLog(Join(target, L"setup.log"));
  Log(L"Skate 2 PC setup %ls%ls", SKATE2_SETUP_VERSION_WSTR, IsWine() ? L" (Wine)" : L"");

  auto job = std::make_shared<InstallJob>();
  job->target = target;
  job->payload = state.payload;
  job->disc = state.disc;
  job->desktop_shortcut = IsDlgButtonChecked(dialog, IDC_DESKTOP) == BST_CHECKED;
  job->start_menu_shortcut = IsDlgButtonChecked(dialog, IDC_STARTMENU) == BST_CHECKED;

  state.progress = std::make_unique<InstallProgress>();
  state.working = true;
  state.finished = false;
  state.error.clear();
  ShowWindow(GetDlgItem(dialog, IDC_PROGRESS), SW_SHOW);
  UpdateUi(dialog, state);
  SetTimer(dialog, kTimerProgress, 150, nullptr);
  InstallProgress* progress = state.progress.get();
  SetupState* state_ptr = &state;
  state.worker = std::thread([dialog, job, progress, state_ptr] {
    std::wstring error;
    const InstallResult result = RunInstall(*job, *progress, error);
    state_ptr->error = error;
    PostMessageW(dialog, kMsgInstallDone, WPARAM(result), 0);
  });
}

void FinishInstall(HWND dialog, SetupState& state, InstallResult result) {
  KillTimer(dialog, kTimerProgress);
  if (state.worker.joinable()) state.worker.join();
  state.working = false;
  state.result = result;
  const std::wstring target = TargetOf(dialog, state);
  std::wstring text;
  if (result == InstallResult::kDone) {
    state.finished = true;
    SendDlgItemMessageW(dialog, IDC_PROGRESS, PBM_SETPOS, 1000, 0);
    text = L"Skate 2 is installed. Start it with " + Join(target, kLauncherName) + L".";
    const std::wstring unix_path = UnixPath(Join(target, kLauncherName));
    if (!unix_path.empty()) {
      text = L"Installed. For Steam (Game Mode): add this file as a non-Steam game and force "
             L"Proton in its compatibility settings:\n" + unix_path;
    }
    // A fresh disc check is not needed for Play / Update any more.
    state.disc.reset();
    state.checked_iso.clear();
    SetDlgItemTextW(dialog, IDC_ISO, L"");
  } else if (result == InstallResult::kCancelled) {
    text = L"Cancelled. Run the setup again with the same folder to continue where it stopped.";
  } else {
    text = L"Install failed: " + state.error;
  }
  SetDlgItemTextW(dialog, IDC_STATUS, text.c_str());
  UpdateUi(dialog, state);
  if (result == InstallResult::kDone) SetFocus(GetDlgItem(dialog, IDC_PLAY));
}

INT_PTR CALLBACK SetupProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<SetupState*>(GetWindowLongPtrW(dialog, DWLP_USER));
  switch (message) {
    case WM_INITDIALOG: {
      state = reinterpret_cast<SetupState*>(lparam);
      SetWindowLongPtrW(dialog, DWLP_USER, LONG_PTR(state));
      HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
      if (icon) {
        SendMessageW(dialog, WM_SETICON, ICON_BIG, LPARAM(icon));
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, LPARAM(icon));
      }
      std::wstring title = L"Skate 2 PC Setup ";
      title += SKATE2_SETUP_VERSION_WSTR;
      SetWindowTextW(dialog, title.c_str());
      SendDlgItemMessageW(dialog, IDC_PROGRESS, PBM_SETRANGE32, 0, 1000);
      ShowWindow(GetDlgItem(dialog, IDC_PROGRESS), SW_HIDE);

      std::wstring dir = state->options->dir;
      if (!state->fixed_dir.empty()) {
        dir = state->fixed_dir;
      } else if (dir.empty()) {
        dir = RegistryRead(L"InstallDir");
        if (dir.empty() || !IsDir(dir)) dir = DefaultInstallDir();
      }
      SetDlgItemTextW(dialog, IDC_DIR, dir.c_str());
      SetDlgItemTextW(dialog, IDC_ISO, state->options->iso.c_str());
      // Shortcuts make no sense in a Proton prefix, Steam starts the game.
      const bool shortcuts = !IsWine() && state->payload;
      ShowWindow(GetDlgItem(dialog, IDC_DESKTOP), shortcuts ? SW_SHOW : SW_HIDE);
      ShowWindow(GetDlgItem(dialog, IDC_STARTMENU), shortcuts ? SW_SHOW : SW_HIDE);
      CheckDlgButton(dialog, IDC_DESKTOP, BST_CHECKED);
      CheckDlgButton(dialog, IDC_STARTMENU, BST_CHECKED);
      if (!state->payload) {
        SetDlgItemTextW(dialog, IDC_INTRO,
                        L"The game files are missing in this folder. Select your own Xbox 360 "
                        L"Skate 2 disc image to copy them from.");
      }
      CheckIso(dialog, *state);
      UpdateUi(dialog, *state);
      return TRUE;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      const int code = HIWORD(wparam);
      if (id == IDC_ISO && code == EN_CHANGE) {
        SetTimer(dialog, kTimerCheckIso, 400, nullptr);
      } else if (id == IDC_DIR && code == EN_CHANGE) {
        UpdateUi(dialog, *state);
      } else if (id == IDC_ISO_BROWSE) {
        const std::wstring current = GetText(dialog, IDC_ISO);
        const std::wstring path = PickPath(dialog, false, current.empty() ? L"" : DirName(current));
        if (!path.empty()) {
          SetDlgItemTextW(dialog, IDC_ISO, path.c_str());
          KillTimer(dialog, kTimerCheckIso);
          CheckIso(dialog, *state);
          UpdateUi(dialog, *state);
        }
      } else if (id == IDC_DIR_BROWSE) {
        const std::wstring path = PickPath(dialog, true, GetText(dialog, IDC_DIR));
        if (!path.empty()) SetDlgItemTextW(dialog, IDC_DIR, path.c_str());
      } else if (id == IDC_INSTALL) {
        KillTimer(dialog, kTimerCheckIso);
        CheckIso(dialog, *state);
        UpdateUi(dialog, *state);
        if (IsWindowEnabled(GetDlgItem(dialog, IDC_INSTALL))) StartInstall(dialog, *state);
      } else if (id == IDC_PLAY) {
        state->play_dir = TargetOf(dialog, *state);
        EndDialog(dialog, kExitPlay);
      } else if (id == IDC_OPTIONS) {
        EditOptions(dialog, TargetOf(dialog, *state), false);
      } else if (id == IDCANCEL) {
        if (state->working) {
          state->progress->cancel = true;
          ShowProgress(dialog, *state);
        } else {
          EndDialog(dialog, IDCANCEL);
        }
      }
      return TRUE;
    }
    case WM_TIMER:
      if (wparam == kTimerProgress && state->working) ShowProgress(dialog, *state);
      if (wparam == kTimerCheckIso) {
        KillTimer(dialog, kTimerCheckIso);
        CheckIso(dialog, *state);
        UpdateUi(dialog, *state);
      }
      return TRUE;
    case kMsgInstallDone:
      FinishInstall(dialog, *state, InstallResult(wparam));
      return TRUE;
    case WM_DROPFILES: {
      HDROP drop = HDROP(wparam);
      wchar_t path[MAX_PATH * 4];
      if (!state->working && DragQueryFileW(drop, 0, path, MAX_PATH * 4)) {
        // A folder with the game files is a source; any other folder is
        // where to install.
        const bool game_folder = IsFile(Join(path, L"default.xex"));
        if (IsDir(path) && !game_folder) {
          if (state->fixed_dir.empty()) SetDlgItemTextW(dialog, IDC_DIR, path);
        } else {
          SetDlgItemTextW(dialog, IDC_ISO, path);
          KillTimer(dialog, kTimerCheckIso);
          CheckIso(dialog, *state);
          UpdateUi(dialog, *state);
        }
      }
      DragFinish(drop);
      return TRUE;
    }
    case WM_CLOSE:
      SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      return TRUE;
  }
  return FALSE;
}

// --- entry points ------------------------------------------------------------

int Launch(const std::wstring& dir, const Options& options) {
  const std::wstring log = Join(dir, L"launcher.log");
  DeleteFileW(log.c_str());
  OpenLog(log);
  Log(L"Skate 2 PC launcher %ls%ls", SKATE2_SETUP_VERSION_WSTR, IsWine() ? L" (Wine)" : L"");
  const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  if (options.options || shift || LoadSettings(dir).show_options) {
    if (!EditOptions(nullptr, dir, true)) return 0;
  }
  LaunchSettings settings = LoadSettings(dir);
  // Upstream launcher arguments, for game-library entries: not saved.
  if (options.render_scale) settings.render_scale = *options.render_scale;
  if (options.frame_rate) {
    for (size_t i = 0; i < kFrameRateCount; ++i) {
      if (*options.frame_rate == kFrameRates[i]) settings.frame_rate = kFrameRates[i];
    }
  }
  if (options.windowed) settings.windowed = true;
  return RunGame(dir, settings, options.passthrough, nullptr);
}

int QuietInstall(const Options& options, const Payload* payload) {
  if (options.dir.empty()) return 2;
  const std::wstring target = InstallTarget(options.dir);
  if (!IsAsciiPath(target)) return 2;
  MakeDirs(target);
  OpenLog(Join(target, L"setup.log"));
  Log(L"Skate 2 PC setup %ls, quiet install", SKATE2_SETUP_VERSION_WSTR);
  InstallJob job;
  job.target = target;
  job.payload = payload;
  if (!options.iso.empty()) {
    Disc disc;
    if (std::wstring error = CheckDisc(options.iso, disc); !error.empty()) {
      Log(L"Disc rejected: %ls", error.c_str());
      return 3;
    }
    job.disc = std::move(disc);
  }
  InstallProgress progress;
  std::wstring error;
  return RunInstall(job, progress, error) == InstallResult::kDone ? 0 : 1;
}

int RunSetup(const Options& options, const Payload* payload, const std::wstring& fixed_dir) {
  SetupState state;
  state.options = &options;
  state.payload = payload;
  state.fixed_dir = fixed_dir;
  const INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETUP),
                                         nullptr, SetupProc, LPARAM(&state));
  if (state.worker.joinable()) state.worker.join();
  if (result == kExitPlay && IsInstalled(state.play_dir)) {
    Options play = options;
    play.options = false;
    return Launch(state.play_dir, play);
  }
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const Options options = ParseOptions();
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&controls);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  Payload payload;
  const bool is_setup = ReadPayload(payload);
  const std::wstring self_dir = DirName(ExePath());
  int code = 0;
  if (is_setup) {
    const std::wstring installed = RegistryRead(L"InstallDir");
    if (options.quiet) {
      code = QuietInstall(options, &payload);
    } else if (options.play && IsInstalled(installed)) {
      code = Launch(installed, options);
    } else {
      code = RunSetup(options, &payload, {});
    }
  } else if (IsInstalled(self_dir) && !options.setup) {
    code = Launch(self_dir, options);
  } else if (IsFile(Join(self_dir, kGameExeName))) {
    // Program files without the game files: copy them from the disc here.
    if (options.quiet) {
      Options quiet = options;
      quiet.dir = self_dir;
      code = QuietInstall(quiet, nullptr);
    } else {
      code = RunSetup(options, nullptr, self_dir);
    }
  } else {
    MessageBoxW(nullptr,
                L"skate2.exe was not found next to this launcher. Keep Launch Skate 2.exe in the "
                L"folder Skate 2 was installed to, or install the game again with "
                L"Skate2-PC-Setup.exe.",
                L"Skate 2", MB_ICONERROR);
    code = 1;
  }
  CoUninitialize();
  return code;
}
