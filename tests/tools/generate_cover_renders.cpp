#include "src/generated_cover.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int
main(int argc, char **argv) {
#ifndef _WIN32
  (void) argc;
  (void) argv;
  std::cerr << "Windows renderer required\n";
  return 77;
#else
  if (argc != 2 && argc != 3) {
    std::cerr << "usage: generate_cover_renders <output-directory> [commands-file: ten lines, one command per name]\n";
    return 2;
  }
  const std::filesystem::path output = argv[1];
  const std::filesystem::path cache = output / "cache";
  std::filesystem::create_directories(output);
  const std::array names {
    "桌面",
    "Steam Big Picture",
    "原神",
    "崩坏：星穹铁道",
    "Microsoft Edge",
    "The Legend of Zelda: Breath of the Wild – Definitive Edition Collector's Bundle",
    "ファイナルファンタジーXIV",
    "🎮 Game Night 🔥",
    "X",
    "Google Chrome",
  };
  // the built-in programs of Windows have icons of their own; the launchers and the empty command have none the cover may use
  std::array<std::string, 10> commands {
    "",                                                  // 桌面: the Desktop application (Explorer's icon)
    R"(C:\Windows\System32\mstsc.exe)",                 // Steam Big Picture
    R"(C:\Windows\System32\msinfo32.exe)",               // 原神
    "",                                                  // 崩坏：星穹铁道: nothing to take an icon from
    R"(C:\Windows\System32\perfmon.exe)",               // Microsoft Edge
    R"(cmd.exe /c start "" zelda)",                      // The Legend of Zelda: a launcher
    R"(powershell.exe -NoProfile -Command game)",        // ファイナルファンタジーXIV: a launcher
    R"(notepad.exe)",                                    // Game Night: a generic program
    R"(rundll32.exe shell32.dll,Control_RunDLL)",        // X: a launcher
    R"(C:\Windows\System32\charmap.exe)",               // Google Chrome
  };
  if (argc == 3) {
    std::ifstream file(argv[2]);
    for (auto &command : commands) {
      if (!std::getline(file, command)) break;
      while (!command.empty() && (command.back() == '\r' || command.back() == '\n')) command.pop_back();
    }
  }
  const std::array styles { "tonal", "monogram", "icon", "editorial", "glass", "geometric" };
  const std::array orientations { generated_cover::orientation_e::portrait, generated_cover::orientation_e::landscape };
  for (std::size_t name_index = 0; name_index < names.size(); ++name_index) {
    generated_cover::app_t app;
    app.id = std::to_string(name_index + 1);
    app.name = names[name_index];
    app.command = commands[name_index];
    if (name_index == 0) app.image_path = "desktop.png";
    for (const auto style_name : styles) {
      const auto style = generated_cover::style_from_name(style_name);
      for (const auto orientation : orientations) {
        const auto asset = generated_cover::preview(app, style, orientation, cache);
        if (asset.path.empty()) {
          std::cerr << "render failed for " << name_index << ' ' << style_name << '\n';
          return 1;
        }
        const auto directory = output / style_name / generated_cover::orientation_name(orientation);
        std::filesystem::create_directories(directory);
        const auto target = directory / (std::to_string(name_index + 1) + asset.path.extension().string());
        std::error_code error;
        std::filesystem::copy_file(asset.path, target, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
          std::cerr << "copy failed for " << target.string() << ": " << error.message() << '\n';
          return 1;
        }
      }
    }
  }
  std::ofstream manifest(output / "manifest.txt");
  for (std::size_t index = 0; index < names.size(); ++index) manifest << index + 1 << '\t' << names[index] << '\n';
  return 0;
#endif
}
