// Compatibility table and component report contracts. Runs on any machine:
// the report must describe whatever is installed without trusting it.
#include "components.h"
#include <iostream>
#include <stdexcept>
static void
Check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
int
main() {
  try {
    using mat_compat::dolby_directory_shape;
    Check(dolby_directory_shape(L"C:\\Program Files\\WindowsApps\\DolbyLaboratories.DolbyAccess_3.27.12250.0_x64__rz1tebttyb220\\DLLs\\x64\\"), "pinned install path");
    Check(dolby_directory_shape(L"D:\\WindowsApps\\DolbyLaboratories.DolbyAccess_3.28.1.0_x64__rz1tebttyb220\\DLLs\\x64\\"), "other drive and version");
    for (auto bad : {
           L"",
           L"C:\\Program Files\\WindowsApps\\DolbyLaboratories.DolbyAccess_3.27.12250.0_x64__rz1tebttyb220\\DLLs\\x64",
           L"C:\\Program Files\\WindowsApps\\DolbyLaboratories.DolbyAccess_3.27.12250.0_x86__rz1tebttyb220\\DLLs\\x64\\",
           L"C:\\Program Files\\WindowsApps\\DolbyLaboratories.DolbyAccess__x64__rz1tebttyb220\\DLLs\\x64\\",
           L"C:\\Program Files\\WindowsApps\\DolbyLaboratories.DolbyAccess_3.27\\..\\x_x64__rz1tebttyb220\\DLLs\\x64\\",
           L"C:\\Users\\me\\WindowsApps\\DolbyLaboratories.DolbyAccess_3.27.12250.0_x64__otherpublisher\\DLLs\\x64\\",
           L"\\\\server\\share\\WindowsApps\\DolbyLaboratories.DolbyAccess_3.27.12250.0_x64__rz1tebttyb220\\DLLs\\x64\\",
           L"C:/Program Files/WindowsApps/DolbyLaboratories.DolbyAccess_3.27.12250.0_x64__rz1tebttyb220/DLLs/x64/",
         }) {
      Check(!dolby_directory_shape(bad), "rejected directory shape");
    }
    Check(mat_compat::supported_audioeng(mat_compat::audioeng_sha256[0]) && !mat_compat::supported_audioeng(std::string(64, '0')), "audioeng table");
    const auto &pinned = mat_compat::dolby_builds[0];
    Check(mat_compat::supported_dolby(pinned.encoder_sha256, pinned.engine_sha256) && !mat_compat::supported_dolby(pinned.encoder_sha256, pinned.encoder_sha256), "Dolby pairs are matched together");

    const auto engine = mat_compat::audio_engine();
    Check(engine.support != mat_compat::Support::missing && engine.sha256.size() == 64 && engine.version != "unknown", "system audioeng.dll is described");
    Check(engine.support == (mat_compat::supported_audioeng(engine.sha256) ? mat_compat::Support::supported : mat_compat::Support::unsupported), "audioeng support follows the table");
    const auto dolby = sonic_live::find_dolby_access();
    Check(dolby.component.support == mat_compat::Support::missing || !dolby.directory.empty(), "installed Dolby Access has a directory");
    const bool ready = sonic_live::report_components(engine, dolby.component);
    Check(ready == (engine.support == mat_compat::Support::supported && dolby.component.support == mat_compat::Support::supported), "report result");
    std::cout << "PASS MAT compatibility table, Dolby package path shape and read-only component report\n";
    return 0;
  }
  catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
