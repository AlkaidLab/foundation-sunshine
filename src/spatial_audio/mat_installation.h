#pragma once
#include "native_mat_guard.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

namespace spatial_audio {
  inline bool
  mat_protected_acl(PSID owner, PACL acl) {
    auto trusted = [](PSID sid) {
      LPWSTR text = nullptr;
      if (!ConvertSidToStringSidW(sid, &text)) return false;
      const std::wstring value(text);
      LocalFree(text);
      return value == L"S-1-5-18" || value == L"S-1-5-32-544" ||
             value == L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";
    };
    bool okay = owner && trusted(owner) && acl;
    constexpr DWORD writes = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
                             FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
    for (DWORD i = 0; okay && i < acl->AceCount; ++i) {
      void *entry = nullptr;
      okay = GetAce(acl, i, &entry);
      if (!okay) break;
      const auto *header = static_cast<ACE_HEADER *>(entry);
      if (header->AceFlags & INHERIT_ONLY_ACE) continue;
      if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
        const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(entry);
        if ((ace->Mask & writes) && !trusted((PSID) &ace->SidStart)) okay = false;
      }
      else if (header->AceType != ACCESS_DENIED_ACE_TYPE) {
        okay = false;  // Do not interpret unfamiliar conditional/object grants.
      }
    }
    return okay;
  }

  // Installed capture code runs elevated. Every ancestor must retain the
  // installer's trust boundary; being somewhere under Program Files is not enough.
  inline bool
  mat_protected_entry(const std::filesystem::path &path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
          &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    const bool okay = mat_protected_acl(owner, acl);
    LocalFree(descriptor);
    return okay;
  }

  inline bool
  mat_installed_image(const std::filesystem::path &image) {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &folder))) return false;
    const std::filesystem::path programs(folder);
    CoTaskMemFree(folder);
    if (!image.is_absolute() || _wcsicmp(image.filename().c_str(), L"sunshine-plugin-mat-capture.exe") ||
        _wcsicmp(image.parent_path().filename().c_str(), L"mat-capture") || _wcsicmp(image.parent_path().parent_path().filename().c_str(), L"tools")) return false;
    auto path = image;
    while (!path.empty() && _wcsicmp(path.c_str(), programs.c_str())) {
      if (!mat_protected_entry(path)) return false;
      const auto parent = path.parent_path();
      if (parent == path) return false;
      path = parent;
    }
    return !path.empty() && !_wcsicmp(path.c_str(), programs.c_str()) && mat_protected_entry(programs);
  }
}  // namespace spatial_audio
