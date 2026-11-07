/**
 * @file src/default_app_names.h
 * @brief The names of the built-in entries of apps.json in every language of the host's `locale` setting.
 *
 * Header-only and free of host dependencies, so that the tests can use it alone. The source is UTF-8 (the host is built with GCC,
 * whose source and execution character sets are UTF-8).
 *
 * Steam keeps its own wording: the second word of the name is what Steam calls the mode in that language.
 * The entries other than English were written for this table and are meant to be reviewed by a native speaker.
 */
#pragma once

// standard includes
#include <array>
#include <string>
#include <string_view>

namespace default_apps::names {
  using namespace std::literals;

  /// The locales of the host setting (config.cpp, `locale`), in the order of the columns of the tables below.
  inline constexpr std::array<std::string_view, 19> locales {
    "en"sv, "bg"sv, "cs"sv, "de"sv, "es"sv, "fr"sv, "it"sv, "ja"sv, "ko"sv, "pl"sv,
    "pt"sv, "pt_BR"sv, "ru"sv, "sv"sv, "tr"sv, "uk"sv, "zh"sv, "zh_TW"sv, "en_GB"sv
  };

  struct entry_t {
    std::string_view id;
    std::array<std::string_view, 19> text;
  };

  /// The entries of apps.json that the host ships (the id is the one of `default-id`), and the two commands of the Desktop entry (their `id` of menu-cmd).
  // clang-format off
  inline constexpr std::array<entry_t, 6> table {{
    { "desktop"sv, { "Desktop"sv, "Работен плот"sv, "Plocha"sv, "Desktop"sv, "Escritorio"sv, "Bureau"sv, "Desktop"sv, "デスクトップ"sv, "데스크톱"sv, "Pulpit"sv,
                     "Ambiente de trabalho"sv, "Área de trabalho"sv, "Рабочий стол"sv, "Skrivbord"sv, "Masaüstü"sv, "Робочий стіл"sv, "桌面"sv, "桌面"sv, "Desktop"sv } },
    { "steam"sv, { "Steam Big Picture"sv, "Steam Режим Big Picture"sv, "Steam Režim Big Picture"sv, "Steam Big-Picture-Modus"sv, "Steam Modo Big Picture"sv, "Steam Mode Big Picture"sv,
                   "Steam Modalità Big Picture"sv, "Steam Big Picture モード"sv, "Steam Big Picture 모드"sv, "Steam Tryb Big Picture"sv, "Steam Modo Big Picture"sv,
                   "Steam Modo Big Picture"sv, "Steam Режим Big Picture"sv, "Steam Big Picture-läge"sv, "Steam Big Picture Modu"sv, "Steam Режим Big Picture"sv,
                   "Steam Big Picture 模式"sv, "Steam Big Picture 模式"sv, "Steam Big Picture"sv } },
    { "xbox"sv, { "Xbox Game"sv, "Xbox игра"sv, "Hra Xbox"sv, "Xbox-Spiel"sv, "Juego de Xbox"sv, "Jeu Xbox"sv, "Gioco Xbox"sv, "Xbox ゲーム"sv, "Xbox 게임"sv, "Gra Xbox"sv,
                  "Jogo Xbox"sv, "Jogo Xbox"sv, "Игра Xbox"sv, "Xbox-spel"sv, "Xbox Oyunu"sv, "Гра Xbox"sv, "Xbox 游戏"sv, "Xbox 遊戲"sv, "Xbox Game"sv } },
    { "big-screen"sv, { "Big Screen"sv, "Голям екран"sv, "Velká obrazovka"sv, "Großbild"sv, "Pantalla grande"sv, "Grand écran"sv, "Schermo grande"sv, "ビッグスクリーン"sv, "빅 스크린"sv, "Duży ekran"sv,
                        "Ecrã grande"sv, "Tela grande"sv, "Большой экран"sv, "Stor skärm"sv, "Büyük ekran"sv, "Великий екран"sv, "大屏"sv, "大螢幕"sv, "Big Screen"sv } },
    { "kcENAT5r9P"sv, { "Touch Keyboard"sv, "Сензорна клавиатура"sv, "Dotyková klávesnice"sv, "Bildschirmtastatur"sv, "Teclado táctil"sv, "Clavier tactile"sv, "Tastiera touch"sv, "タッチキーボード"sv, "터치 키보드"sv, "Klawiatura dotykowa"sv,
                        "Teclado tátil"sv, "Teclado virtual"sv, "Сенсорная клавиатура"sv, "Pekskärmstangentbord"sv, "Dokunmatik klavye"sv, "Сенсорна клавіатура"sv, "触摸键盘"sv, "觸控鍵盤"sv, "Touch Keyboard"sv } },
    { "rjeOKHmcdL"sv, { "Desktop Pet"sv, "Работен домашен любимец"sv, "Plochový mazlíček"sv, "Desktop-Begleiter"sv, "Mascota de escritorio"sv, "Mascotte de bureau"sv, "Mascotte del desktop"sv, "デスクトップペット"sv, "데스크톱 펫"sv, "Maskotka pulpitu"sv,
                        "Mascote do ambiente de trabalho"sv, "Mascote da área de trabalho"sv, "Питомец на рабочем столе"sv, "Skrivbordshusdjur"sv, "Masaüstü Evcil Hayvanı"sv, "Улюбленець на робочому столі"sv, "桌宠"sv, "桌寵"sv, "Desktop Pet"sv } },
  }};
  // clang-format on

  /// The column of a locale setting: the code itself, else its language ("pt_PT" -> "pt", "zh-Hans" -> "zh", "de_AT" -> "de"; Traditional Chinese is only "zh_TW", "zh_HK" and "zh-Hant" included), else English.
  inline std::size_t
  column_of(std::string_view locale) {
    std::string code { locale };
    for (auto &character : code) {
      if (character == '-') character = '_';
    }
    for (std::size_t index = 0; index < locales.size(); ++index) {
      if (locales[index] == code) return index;
    }
    if (code == "zh_HK" || code == "zh_MO" || code == "zh_Hant" || code.rfind("zh_Hant", 0) == 0) return 17;
    const auto separator = code.find('_');
    const std::string_view language = separator == std::string::npos ? std::string_view { code } : std::string_view { code }.substr(0, separator);
    for (std::size_t index = 0; index < locales.size(); ++index) {
      if (locales[index] == language) return index;
    }
    return 0;
  }

  /// The entry of an id, or null.
  inline const entry_t *
  find(std::string_view id) {
    for (const auto &entry : table) {
      if (entry.id == id) return &entry;
    }
    return nullptr;
  }

  /// The name of an entry in a locale ("" for an id that is not in the table).
  inline std::string
  name_in(std::string_view id, std::string_view locale) {
    const auto *entry = find(id);
    return entry ? std::string { entry->text[column_of(locale)] } : std::string {};
  }

  /// The English name of an entry: the one that its application id is made from, so a change of language does not change the id.
  inline std::string
  canonical_name(std::string_view id) {
    return name_in(id, "en");
  }

  /// Whether the text is the name of the entry in any language.
  inline bool
  is_known(std::string_view id, std::string_view text) {
    const auto *entry = find(id);
    if (!entry) return false;
    for (const auto candidate : entry->text) {
      if (candidate == text) return true;
    }
    return false;
  }
}  // namespace default_apps::names
