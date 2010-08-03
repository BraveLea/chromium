// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <windows.h>
#include <msi.h>
#include <shellapi.h>
#include <shlobj.h>

#include "base/at_exit.h"
#include "base/basictypes.h"
#include "base/command_line.h"
#include "base/file_util.h"
#include "base/path_service.h"
#include "base/process_util.h"
#include "base/registry.h"
#include "base/scoped_handle_win.h"
#include "base/string_number_conversions.h"
#include "base/string_util.h"
#include "base/utf_string_conversions.h"
#include "base/win_util.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/installer/setup/install.h"
#include "chrome/installer/setup/setup_constants.h"
#include "chrome/installer/setup/setup_util.h"
#include "chrome/installer/setup/uninstall.h"
#include "chrome/installer/util/browser_distribution.h"
#include "chrome/installer/util/delete_after_reboot_helper.h"
#include "chrome/installer/util/delete_tree_work_item.h"
#include "chrome/installer/util/helper.h"
#include "chrome/installer/util/html_dialog.h"
#include "chrome/installer/util/install_util.h"
#include "chrome/installer/util/l10n_string_util.h"
#include "chrome/installer/util/logging_installer.h"
#include "chrome/installer/util/lzma_util.h"
#include "chrome/installer/util/google_update_settings.h"
#include "chrome/installer/util/google_update_constants.h"
#include "chrome/installer/util/master_preferences.h"
#include "chrome/installer/util/shell_util.h"
#include "chrome/installer/util/util_constants.h"

#include "installer_util_strings.h"

namespace {

// This method unpacks and uncompresses the given archive file. For Chrome
// install we are creating a uncompressed archive that contains all the files
// needed for the installer. This uncompressed archive is later compressed.
//
// This method first uncompresses archive specified by parameter "archive"
// and assumes that it will result in an uncompressed full archive file
// (chrome.7z) or uncompressed archive patch file (chrome_patch.diff). If it
// is patch file, it is applied to the old archive file that should be
// present on the system already. As the final step the new archive file
// is unpacked in the path specified by parameter "path".
DWORD UnPackArchive(const std::wstring& archive, bool system_install,
                    const installer::Version* installed_version,
                    const std::wstring& temp_path, const std::wstring& path,
                    bool& incremental_install) {
  // First uncompress the payload. This could be a differential
  // update (patch.7z) or full archive (chrome.7z). If this uncompress fails
  // return with error.
  std::wstring unpacked_file;
  int32 ret = LzmaUtil::UnPackArchive(archive, temp_path, &unpacked_file);
  if (ret != NO_ERROR)
    return ret;

  std::wstring uncompressed_archive(temp_path);
  file_util::AppendToPath(&uncompressed_archive, installer::kChromeArchive);

  // Check if this is differential update and if it is, patch it to the
  // installer archive that should already be on the machine. We assume
  // it is a differential installer if chrome.7z is not found.
  if (!file_util::PathExists(FilePath::FromWStringHack(uncompressed_archive))) {
    incremental_install = true;
    LOG(INFO) << "Differential patch found. Applying to existing archive.";
    if (!installed_version) {
      LOG(ERROR) << "Can not use differential update when Chrome is not "
                 << "installed on the system.";
      return installer_util::CHROME_NOT_INSTALLED;
    }
    std::wstring existing_archive =
        installer::GetChromeInstallPath(system_install);
    file_util::AppendToPath(&existing_archive,
                            installed_version->GetString());
    file_util::AppendToPath(&existing_archive, installer_util::kInstallerDir);
    file_util::AppendToPath(&existing_archive, installer::kChromeArchive);
    if (int i = setup_util::ApplyDiffPatch(existing_archive, unpacked_file,
                                           uncompressed_archive)) {
      LOG(ERROR) << "Binary patching failed with error " << i;
      return i;
    }
  }

  // Unpack the uncompressed archive.
  return LzmaUtil::UnPackArchive(uncompressed_archive, path, &unpacked_file);
}


// This function is called when --rename-chrome-exe option is specified on
// setup.exe command line. This function assumes an in-use update has happened
// for Chrome so there should be a file called new_chrome.exe on the file
// system and a key called 'opv' in the registry. This function will move
// new_chrome.exe to chrome.exe and delete 'opv' key in one atomic operation.
installer_util::InstallStatus RenameChromeExecutables(bool system_install) {
  std::wstring chrome_path(installer::GetChromeInstallPath(system_install));

  std::wstring chrome_exe(chrome_path);
  file_util::AppendToPath(&chrome_exe, installer_util::kChromeExe);
  std::wstring chrome_old_exe(chrome_path);
  file_util::AppendToPath(&chrome_old_exe, installer_util::kChromeOldExe);
  std::wstring chrome_new_exe(chrome_path);
  file_util::AppendToPath(&chrome_new_exe, installer_util::kChromeNewExe);

  scoped_ptr<WorkItemList> install_list(WorkItem::CreateWorkItemList());
  install_list->AddDeleteTreeWorkItem(chrome_old_exe, std::wstring());
  FilePath temp_path;
  if (!file_util::CreateNewTempDirectory(L"chrome_", &temp_path)) {
    LOG(ERROR) << "Failed to create Temp directory " << temp_path.value();
    return installer_util::RENAME_FAILED;
  }
  install_list->AddCopyTreeWorkItem(chrome_new_exe,
                                    chrome_exe,
                                    temp_path.ToWStringHack(),
                                    WorkItem::IF_DIFFERENT,
                                    std::wstring());
  HKEY reg_root = system_install ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  BrowserDistribution *dist = BrowserDistribution::GetDistribution();
  install_list->AddDeleteRegValueWorkItem(reg_root,
                                          dist->GetVersionKey(),
                                          google_update::kRegOldVersionField,
                                          true);
  install_list->AddDeleteTreeWorkItem(chrome_new_exe, std::wstring());
  install_list->AddDeleteRegValueWorkItem(reg_root,
                                          dist->GetVersionKey(),
                                          google_update::kRegRenameCmdField,
                                          true);
  installer_util::InstallStatus ret = installer_util::RENAME_SUCCESSFUL;
  if (!install_list->Do()) {
    LOG(ERROR) << "Renaming of executables failed. Rolling back any changes.";
    install_list->Rollback();
    ret = installer_util::RENAME_FAILED;
  }
  file_util::Delete(temp_path, true);
  return ret;
}

bool CheckPreInstallConditions(const installer::Version* installed_version,
                               bool system_install,
                               installer_util::InstallStatus& status) {
  bool is_first_install = (NULL == installed_version);
  // Check to avoid simultaneous per-user and per-machine installs.
  scoped_ptr<installer::Version>
      chrome_version(InstallUtil::GetChromeVersion(!system_install));
  if (chrome_version.get()) {
    LOG(ERROR) << "Already installed version " << chrome_version->GetString()
               << " conflicts with the current install mode.";
    if (!system_install && is_first_install) {
      // This is user-level install and there is a system-level chrome
      // installation. Instruct omaha to launch the existing one. There
      // should be no error dialog.
      std::wstring chrome_exe(installer::GetChromeInstallPath(!system_install));
      if (chrome_exe.empty()) {
        // If we failed to construct install path. Give up.
        status = installer_util::OS_ERROR;
        InstallUtil::WriteInstallerResult(system_install, status,
                                          IDS_INSTALL_OS_ERROR_BASE, NULL);
        return false;
      } else {
        status = installer_util::EXISTING_VERSION_LAUNCHED;
        file_util::AppendToPath(&chrome_exe, installer_util::kChromeExe);
        chrome_exe = L"\"" + chrome_exe + L"\" --"
                     + ASCIIToWide(switches::kFirstRun);
        InstallUtil::WriteInstallerResult(system_install, status,
                                          0, NULL);
        LOG(INFO) << "Launching existing system-level chrome instead.";
        base::LaunchApp(chrome_exe, false, false, NULL);
        return false;
      }
    }
    // If the following compile assert fires it means that the InstallStatus
    // enumeration changed which will break the contract between the old chrome
    // installed and the new setup.exe that is trying to upgrade.
    COMPILE_ASSERT(installer_util::SXS_OPTION_NOT_SUPPORTED == 33,
                   dont_change_enum);

    // This is an update, not an install. Omaha should know the difference
    // and not show a dialog.
    status = system_install ? installer_util::USER_LEVEL_INSTALL_EXISTS :
                              installer_util::SYSTEM_LEVEL_INSTALL_EXISTS;
    int str_id = system_install ? IDS_INSTALL_USER_LEVEL_EXISTS_BASE :
                                  IDS_INSTALL_SYSTEM_LEVEL_EXISTS_BASE;
    InstallUtil::WriteInstallerResult(system_install, status, str_id, NULL);
    return false;
  }
  // If no previous installation of Chrome, make sure installation directory
  // either does not exist or can be deleted (i.e. is not locked by some other
  // process).
  if (is_first_install) {
    FilePath install_path = FilePath::FromWStringHack(
        installer::GetChromeInstallPath(system_install));
    if (file_util::PathExists(install_path) &&
        !file_util::Delete(install_path, true)) {
      LOG(ERROR) << "Installation directory " << install_path.value()
                 << " exists and can not be deleted.";
      status = installer_util::INSTALL_DIR_IN_USE;
      int str_id = IDS_INSTALL_DIR_IN_USE_BASE;
      InstallUtil::WriteInstallerResult(system_install, status, str_id, NULL);
      return false;
    }
  }

  return true;
}

installer_util::InstallStatus InstallChrome(const CommandLine& cmd_line,
    const installer::Version* installed_version, const DictionaryValue* prefs) {
  bool system_level = false;
  installer_util::GetDistroBooleanPreference(prefs,
      installer_util::master_preferences::kSystemLevel, &system_level);
  installer_util::InstallStatus install_status = installer_util::UNKNOWN_STATUS;
  if (!CheckPreInstallConditions(installed_version,
                                 system_level, install_status))
    return install_status;

  // For install the default location for chrome.packed.7z is in current
  // folder, so get that value first.
  std::wstring archive = file_util::GetDirectoryFromPath(cmd_line.program());
  file_util::AppendToPath(&archive,
                          std::wstring(installer::kChromeCompressedArchive));

  // If --install-archive is given, get the user specified value
  if (cmd_line.HasSwitch(installer_util::switches::kInstallArchive)) {
    archive = cmd_line.GetSwitchValue(
        installer_util::switches::kInstallArchive);
  }
  LOG(INFO) << "Archive found to install Chrome " << archive;

  // Create a temp folder where we will unpack Chrome archive. If it fails,
  // then we are doomed, so return immediately and no cleanup is required.
  FilePath temp_path;
  if (!file_util::CreateNewTempDirectory(L"chrome_", &temp_path)) {
    LOG(ERROR) << "Could not create temporary path.";
    InstallUtil::WriteInstallerResult(system_level,
                                      installer_util::TEMP_DIR_FAILED,
                                      IDS_INSTALL_TEMP_DIR_FAILED_BASE,
                                      NULL);
    return installer_util::TEMP_DIR_FAILED;
  }
  LOG(INFO) << "created path " << temp_path.value();

  BrowserDistribution* dist = BrowserDistribution::GetDistribution();
  std::wstring unpack_path(temp_path.ToWStringHack());
  file_util::AppendToPath(&unpack_path,
                          std::wstring(installer::kInstallSourceDir));
  bool incremental_install = false;
  if (UnPackArchive(archive, system_level, installed_version,
                    temp_path.ToWStringHack(), unpack_path,
                    incremental_install)) {
    install_status = installer_util::UNCOMPRESSION_FAILED;
    InstallUtil::WriteInstallerResult(system_level, install_status,
                                      IDS_INSTALL_UNCOMPRESSION_FAILED_BASE,
                                      NULL);
  } else {
    LOG(INFO) << "unpacked to " << unpack_path;
    std::wstring src_path(unpack_path);
    file_util::AppendToPath(&src_path,
        std::wstring(installer::kInstallSourceChromeDir));
    scoped_ptr<installer::Version>
        installer_version(setup_util::GetVersionFromDir(src_path));
    if (!installer_version.get()) {
      LOG(ERROR) << "Did not find any valid version in installer.";
      install_status = installer_util::INVALID_ARCHIVE;
      InstallUtil::WriteInstallerResult(system_level, install_status,
                                        IDS_INSTALL_INVALID_ARCHIVE_BASE, NULL);
    } else {
      LOG(INFO) << "version to install: " << installer_version->GetString();
      if (installed_version &&
          installed_version->IsHigherThan(installer_version.get())) {
        LOG(ERROR) << "Higher version is already installed.";
        install_status = installer_util::HIGHER_VERSION_EXISTS;
        InstallUtil::WriteInstallerResult(system_level, install_status,
                                          IDS_INSTALL_HIGHER_VERSION_BASE,
                                          NULL);
      } else {
        // We want to keep uncompressed archive (chrome.7z) that we get after
        // uncompressing and binary patching. Get the location for this file.
        std::wstring archive_to_copy(temp_path.ToWStringHack());
        file_util::AppendToPath(&archive_to_copy, installer::kChromeArchive);
        std::wstring prefs_source_path = cmd_line.GetSwitchValue(
            installer_util::switches::kInstallerData);
        install_status = installer::InstallOrUpdateChrome(
            cmd_line.program(), archive_to_copy, temp_path.ToWStringHack(),
            prefs_source_path, prefs, *installer_version, installed_version);

        int install_msg_base = IDS_INSTALL_FAILED_BASE;
        std::wstring chrome_exe;
        if (install_status == installer_util::SAME_VERSION_REPAIR_FAILED) {
            install_msg_base = IDS_SAME_VERSION_REPAIR_FAILED_BASE;
        } else if (install_status != installer_util::INSTALL_FAILED) {
          chrome_exe = installer::GetChromeInstallPath(system_level);
          if (chrome_exe.empty()) {
            // If we failed to construct install path, it means the OS call to
            // get %ProgramFiles% or %AppData% failed. Report this as failure.
            install_msg_base = IDS_INSTALL_OS_ERROR_BASE;
            install_status = installer_util::OS_ERROR;
          } else {
            file_util::AppendToPath(&chrome_exe, installer_util::kChromeExe);
            chrome_exe = L"\"" + chrome_exe + L"\"";
            install_msg_base = 0;
          }
        }

        bool value = false;
        installer_util::GetDistroBooleanPreference(prefs,
            installer_util::master_preferences::kDoNotRegisterForUpdateLaunch,
            &value);
        bool write_chrome_launch_string = (!value) &&
            (install_status != installer_util::IN_USE_UPDATED);

        InstallUtil::WriteInstallerResult(system_level, install_status,
            install_msg_base, write_chrome_launch_string ? &chrome_exe : NULL);

        if (install_status == installer_util::FIRST_INSTALL_SUCCESS) {
          LOG(INFO) << "First install successful.";
          // We never want to launch Chrome in system level install mode.
          bool do_not_launch_chrome = false;
          installer_util::GetDistroBooleanPreference(prefs,
              installer_util::master_preferences::kDoNotLaunchChrome,
              &do_not_launch_chrome);
          if (!system_level && !do_not_launch_chrome)
            installer::LaunchChrome(system_level);
        } else if ((install_status == installer_util::NEW_VERSION_UPDATED) ||
                   (install_status == installer_util::IN_USE_UPDATED)) {
          installer_setup::RemoveLegacyRegistryKeys();
        }
      }
    }
    // There might be an experiment (for upgrade usually) that needs to happen.
    // An experiment's outcome can include chrome's uninstallation. If that is
    // the case we would not do that directly at this point but in another
    // instance of setup.exe
    //
    // There is another way to reach this same function if this is a system
    // level install. See HandleNonInstallCmdLineOptions().
    dist->LaunchUserExperiment(install_status, *installer_version,
                               system_level);
  }

  // Delete temporary files. These include install temporary directory
  // and master profile file if present. Note that we do not care about rollback
  // here and we schedule for deletion on reboot below if the deletes fail. As
  // such, we do not use DeleteTreeWorkItem.
  LOG(INFO) << "Deleting temporary directory " << temp_path.value();
  bool cleanup_success = file_util::Delete(temp_path, true);
  if (cmd_line.HasSwitch(installer_util::switches::kInstallerData)) {
    std::wstring prefs_path = cmd_line.GetSwitchValue(
        installer_util::switches::kInstallerData);
    cleanup_success = file_util::Delete(prefs_path, true) && cleanup_success;
  }

  // The above cleanup has been observed to fail on several users machines.
  // Specifically, it appears that the temp folder may be locked when we try
  // to delete it. This is Rather Bad in the case where we have failed updates
  // as we end up filling users' disks with large-ish temp files. To mitigate
  // this, if we fail to delete the temp folders, then schedule them for
  // deletion at next reboot.
  if (!cleanup_success) {
    ScheduleDirectoryForDeletion(temp_path.ToWStringHack().c_str());
    if (cmd_line.HasSwitch(installer_util::switches::kInstallerData)) {
      std::wstring prefs_path = cmd_line.GetSwitchValue(
          installer_util::switches::kInstallerData);
      ScheduleDirectoryForDeletion(prefs_path.c_str());
    }
  }

  dist->UpdateDiffInstallStatus(system_level, incremental_install,
                                install_status);
  return install_status;
}

installer_util::InstallStatus UninstallChrome(const CommandLine& cmd_line,
                                              const wchar_t* cmd_params,
                                              const installer::Version* version,
                                              bool system_install) {
  LOG(INFO) << "Uninstalling Chome";
  bool force = cmd_line.HasSwitch(installer_util::switches::kForceUninstall);
  if (!version && !force) {
    LOG(ERROR) << "No Chrome installation found for uninstall.";
    InstallUtil::WriteInstallerResult(system_install,
                                      installer_util::CHROME_NOT_INSTALLED,
                                      IDS_UNINSTALL_FAILED_BASE, NULL);
    return installer_util::CHROME_NOT_INSTALLED;
  }

  bool remove_all = !cmd_line.HasSwitch(
      installer_util::switches::kDoNotRemoveSharedItems);

  return installer_setup::UninstallChrome(cmd_line.program(), system_install,
                                          remove_all, force,
                                          cmd_line, cmd_params);
}

installer_util::InstallStatus ShowEULADialog(const std::wstring& inner_frame) {
  LOG(INFO) << "About to show EULA";
  std::wstring eula_path = installer_util::GetLocalizedEulaResource();
  if (eula_path.empty()) {
    LOG(ERROR) << "No EULA path available";
    return installer_util::EULA_REJECTED;
  }
  // Newer versions of the caller pass an inner frame parameter that must
  // be given to the html page being launched.
  if (!inner_frame.empty()) {
    eula_path += L"?innerframe=";
    eula_path += inner_frame;
  }
  installer::EulaHTMLDialog dlg(eula_path);
  installer::EulaHTMLDialog::Outcome outcome = dlg.ShowModal();
  if (installer::EulaHTMLDialog::REJECTED == outcome) {
    LOG(ERROR) << "EULA rejected or EULA failure";
    return installer_util::EULA_REJECTED;
  }
  if (installer::EulaHTMLDialog::ACCEPTED_OPT_IN == outcome) {
    LOG(INFO) << "EULA accepted (opt-in)";
    return installer_util::EULA_ACCEPTED_OPT_IN;
  }
  LOG(INFO) << "EULA accepted (no opt-in)";
  return installer_util::EULA_ACCEPTED;
}

// This method processes any command line options that make setup.exe do
// various tasks other than installation (renaming chrome.exe, showing eula
// among others). This function returns true if any such command line option
// has been found and processed (so setup.exe should exit at that point).
bool HandleNonInstallCmdLineOptions(const CommandLine& cmd_line,
                                    bool system_install,
                                    int& exit_code) {
  BrowserDistribution* dist = BrowserDistribution::GetDistribution();
  if (cmd_line.HasSwitch(installer_util::switches::kUpdateSetupExe)) {
    installer_util::InstallStatus status = installer_util::SETUP_PATCH_FAILED;
    // If --update-setup-exe command line option is given, we apply the given
    // patch to current exe, and store the resulting binary in the path
    // specified by --new-setup-exe. But we need to first unpack the file
    // given in --update-setup-exe.
    FilePath temp_path;
    if (!file_util::CreateNewTempDirectory(L"chrome_", &temp_path)) {
      LOG(ERROR) << "Could not create temporary path.";
    } else {
      std::wstring setup_patch = cmd_line.GetSwitchValue(
          installer_util::switches::kUpdateSetupExe);
      LOG(INFO) << "Opening archive " << setup_patch;
      std::wstring uncompressed_patch;
      if (LzmaUtil::UnPackArchive(setup_patch, temp_path.ToWStringHack(),
                                  &uncompressed_patch) == NO_ERROR) {
        std::wstring old_setup_exe = cmd_line.program();
        std::wstring new_setup_exe = cmd_line.GetSwitchValue(
            installer_util::switches::kNewSetupExe);
        if (!setup_util::ApplyDiffPatch(old_setup_exe, uncompressed_patch,
                                        new_setup_exe))
          status = installer_util::NEW_VERSION_UPDATED;
      }
    }

    exit_code = dist->GetInstallReturnCode(status);
    if (exit_code) {
      LOG(WARNING) << "setup.exe patching failed.";
      InstallUtil::WriteInstallerResult(system_install, status,
                                        IDS_SETUP_PATCH_FAILED_BASE, NULL);
    }
    file_util::Delete(temp_path, true);
    return true;
  } else if (cmd_line.HasSwitch(installer_util::switches::kShowEula)) {
    // Check if we need to show the EULA. If it is passed as a command line
    // then the dialog is shown and regardless of the outcome setup exits here.
    std::wstring inner_frame =
        cmd_line.GetSwitchValue(installer_util::switches::kShowEula);
    exit_code = ShowEULADialog(inner_frame);
    if (installer_util::EULA_REJECTED != exit_code)
      GoogleUpdateSettings::SetEULAConsent(true);
    return true;
  } else if (cmd_line.HasSwitch(
      installer_util::switches::kRegisterChromeBrowser)) {
    // If --register-chrome-browser option is specified, register all
    // Chrome protocol/file associations as well as register it as a valid
    // browser for Start Menu->Internet shortcut. This option should only
    // be used when setup.exe is launched with admin rights. We do not
    // make any user specific changes in this option.
    std::wstring chrome_exe(cmd_line.GetSwitchValue(
        installer_util::switches::kRegisterChromeBrowser));
    std::wstring suffix;
    if (cmd_line.HasSwitch(
        installer_util::switches::kRegisterChromeBrowserSuffix)) {
      suffix = cmd_line.GetSwitchValue(
          installer_util::switches::kRegisterChromeBrowserSuffix);
    }
    exit_code = ShellUtil::RegisterChromeBrowser(chrome_exe, suffix, false);
    return true;
  } else if (cmd_line.HasSwitch(installer_util::switches::kRenameChromeExe)) {
    // If --rename-chrome-exe is specified, we want to rename the executables
    // and exit.
    exit_code = RenameChromeExecutables(system_install);
    return true;
  } else if (cmd_line.HasSwitch(
      installer_util::switches::kRemoveChromeRegistration)) {
    // This is almost reverse of --register-chrome-browser option above.
    // Here we delete Chrome browser registration. This option should only
    // be used when setup.exe is launched with admin rights. We do not
    // make any user specific changes in this option.
    std::wstring suffix;
    if (cmd_line.HasSwitch(
        installer_util::switches::kRegisterChromeBrowserSuffix)) {
      suffix = cmd_line.GetSwitchValue(
          installer_util::switches::kRegisterChromeBrowserSuffix);
    }
    installer_util::InstallStatus tmp = installer_util::UNKNOWN_STATUS;
    installer_setup::DeleteChromeRegistrationKeys(HKEY_LOCAL_MACHINE,
                                                  suffix, tmp);
    exit_code = tmp;
    return true;
  } else if (cmd_line.HasSwitch(installer_util::switches::kInactiveUserToast)) {
    // Launch the inactive user toast experiment.
    std::wstring flavor =
        cmd_line.GetSwitchValue(installer_util::switches::kInactiveUserToast);
    int flavor_int;
    base::StringToInt(flavor, &flavor_int);
    dist->InactiveUserToastExperiment(flavor_int,
        cmd_line.HasSwitch(installer_util::switches::kSystemLevelToast));
    return true;
  } else if (cmd_line.HasSwitch(installer_util::switches::kSystemLevelToast)) {
    // We started as system-level and have been re-launched as user level
    // to continue with the toast experiment.
    scoped_ptr<installer::Version>
        installed_version(InstallUtil::GetChromeVersion(system_install));
    dist->LaunchUserExperiment(installer_util::REENTRY_SYS_UPDATE,
                               *installed_version, true);
    return true;
  }
  return false;
}

bool ShowRebootDialog() {
  // Get a token for this process.
  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                        &token)) {
    LOG(ERROR) << "Failed to open token.";
    return false;
  }

  // Use a ScopedHandle to keep track of and eventually close our handle.
  // TODO(robertshield): Add a Receive() method to base's ScopedHandle.
  ScopedHandle scoped_handle(token);

  // Get the LUID for the shutdown privilege.
  TOKEN_PRIVILEGES tkp = {0};
  LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);
  tkp.PrivilegeCount = 1;
  tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

  // Get the shutdown privilege for this process.
  AdjustTokenPrivileges(token, FALSE, &tkp, 0,
                        reinterpret_cast<PTOKEN_PRIVILEGES>(NULL), 0);
  if (GetLastError() != ERROR_SUCCESS) {
    LOG(ERROR) << "Unable to get shutdown privileges.";
    return false;
  }

  // Popup a dialog that will prompt to reboot using the default system message.
  // TODO(robertshield): Add a localized, more specific string to the prompt.
  RestartDialog(NULL, NULL, EWX_REBOOT | EWX_FORCEIFHUNG);
  return true;
}

// Class to manage COM initialization and uninitialization
class AutoCom {
 public:
  AutoCom() : initialized_(false) { }
  ~AutoCom() {
    if (initialized_) CoUninitialize();
  }
  bool Init(bool system_install) {
    if (CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) != S_OK) {
      LOG(ERROR) << "COM initialization failed.";
      InstallUtil::WriteInstallerResult(system_install,
                                        installer_util::OS_ERROR,
                                        IDS_INSTALL_OS_ERROR_BASE, NULL);
      return false;
    }
    initialized_ = true;
    return true;
  }

 private:
  bool initialized_;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance,
                    wchar_t* command_line, int show_command) {
  // The exit manager is in charge of calling the dtors of singletons.
  base::AtExitManager exit_manager;
  CommandLine::Init(0, NULL);
  CommandLine* mutable_command_line = CommandLine::ForCurrentProcess();

  if (mutable_command_line->HasSwitch(installer_util::switches::kChromeFrame)) {
    mutable_command_line->AppendSwitch(
        WideToASCII(installer_util::switches::kDoNotCreateShortcuts));
    mutable_command_line->AppendSwitch(
        WideToASCII(installer_util::switches::kDoNotLaunchChrome));
    mutable_command_line->AppendSwitch(
        WideToASCII(installer_util::switches::kDoNotRegisterForUpdateLaunch));
  }

  const CommandLine& parsed_command_line = *CommandLine::ForCurrentProcess();

  installer::InitInstallerLogging(parsed_command_line);
  scoped_ptr<DictionaryValue> prefs(installer_util::GetInstallPreferences(
      parsed_command_line));
  bool value = false;
  if (installer_util::GetDistroBooleanPreference(prefs.get(),
          installer_util::master_preferences::kVerboseLogging, &value) &&
      value)
    logging::SetMinLogLevel(logging::LOG_INFO);

  LOG(INFO) << "Command Line: " << parsed_command_line.command_line_string();

  bool system_install = false;
  installer_util::GetDistroBooleanPreference(prefs.get(),
      installer_util::master_preferences::kSystemLevel, &system_install);
  LOG(INFO) << "system install is " << system_install;

  // Check to make sure current system is WinXP or later. If not, log
  // error message and get out.
  if (!InstallUtil::IsOSSupported()) {
    LOG(ERROR) << "Chrome only supports Windows XP or later.";
    InstallUtil::WriteInstallerResult(system_install,
                                      installer_util::OS_NOT_SUPPORTED,
                                      IDS_INSTALL_OS_NOT_SUPPORTED_BASE, NULL);
    return installer_util::OS_NOT_SUPPORTED;
  }

  // Initialize COM for use later.
  AutoCom auto_com;
  if (!auto_com.Init(system_install)) {
    return installer_util::OS_ERROR;
  }

  // Some command line options don't work with SxS install/uninstall
  if (InstallUtil::IsChromeSxSProcess()) {
    if (system_install ||
        parsed_command_line.HasSwitch(
            installer_util::switches::kForceUninstall) ||
        parsed_command_line.HasSwitch(
            installer_util::switches::kMakeChromeDefault) ||
        parsed_command_line.HasSwitch(
            installer_util::switches::kRegisterChromeBrowser) ||
        parsed_command_line.HasSwitch(
            installer_util::switches::kRemoveChromeRegistration) ||
        parsed_command_line.HasSwitch(
            installer_util::switches::kInactiveUserToast) ||
        parsed_command_line.HasSwitch(
            installer_util::switches::kSystemLevelToast)) {
      return installer_util::SXS_OPTION_NOT_SUPPORTED;
    }
  }

  int exit_code = 0;
  if (HandleNonInstallCmdLineOptions(parsed_command_line, system_install,
                                     exit_code))
    return exit_code;

  if (system_install && !IsUserAnAdmin()) {
    if (win_util::GetWinVersion() >= win_util::WINVERSION_VISTA &&
        !parsed_command_line.HasSwitch(installer_util::switches::kRunAsAdmin)) {
      std::wstring exe = parsed_command_line.program();
      std::wstring params(command_line);
      // Append --run-as-admin flag to let the new instance of setup.exe know
      // that we already tried to launch ourselves as admin.
      params.append(L" --");
      params.append(installer_util::switches::kRunAsAdmin);
      DWORD exit_code = installer_util::UNKNOWN_STATUS;
      InstallUtil::ExecuteExeAsAdmin(exe, params, &exit_code);
      return exit_code;
    } else {
      LOG(ERROR) << "Non admin user can not install system level Chrome.";
      InstallUtil::WriteInstallerResult(system_install,
                                        installer_util::INSUFFICIENT_RIGHTS,
                                        IDS_INSTALL_INSUFFICIENT_RIGHTS_BASE,
                                        NULL);
      return installer_util::INSUFFICIENT_RIGHTS;
    }
  }

  // Check the existing version installed.
  scoped_ptr<installer::Version>
      installed_version(InstallUtil::GetChromeVersion(system_install));
  if (installed_version.get()) {
    LOG(INFO) << "version on the system: " << installed_version->GetString();
  }

  installer_util::InstallStatus install_status = installer_util::UNKNOWN_STATUS;
  // If --uninstall option is given, uninstall chrome
  if (parsed_command_line.HasSwitch(installer_util::switches::kUninstall)) {
    install_status = UninstallChrome(parsed_command_line,
                                     command_line,
                                     installed_version.get(),
                                     system_install);
  // If --uninstall option is not specified, we assume it is install case.
  } else {
    install_status = InstallChrome(parsed_command_line,
                                   installed_version.get(),
                                   prefs.get());
  }

  BrowserDistribution* dist = BrowserDistribution::GetDistribution();

  if (InstallUtil::IsChromeFrameProcess() &&
      !parsed_command_line.HasSwitch(
          installer_util::switches::kForceUninstall)) {
    if (install_status == installer_util::UNINSTALL_REQUIRES_REBOOT) {
      ShowRebootDialog();
    } else if (parsed_command_line.HasSwitch(
        installer_util::switches::kUninstall)) {
      ::MessageBoxW(NULL,
                    installer_util::GetLocalizedString(
                        IDS_UNINSTALL_COMPLETE_BASE).c_str(),
                    dist->GetApplicationName().c_str(),
                    MB_OK);
    }
  }

  int return_code = 0;
  // MSI demands that custom actions always return 0 (ERROR_SUCCESS) or it will
  // rollback the action. If we're uninstalling we want to avoid this, so always
  // report success, squashing any more informative return codes.
  if (!(InstallUtil::IsMSIProcess(system_install) &&
        parsed_command_line.HasSwitch(installer_util::switches::kUninstall))) {
    // Note that we allow the status installer_util::UNINSTALL_REQUIRES_REBOOT
    // to pass through, since this is only returned on uninstall which is never
    // invoked directly by Google Update.
    return_code = dist->GetInstallReturnCode(install_status);
  }

  LOG(INFO) << "Installation complete, returning: " << return_code;
  return return_code;
}
