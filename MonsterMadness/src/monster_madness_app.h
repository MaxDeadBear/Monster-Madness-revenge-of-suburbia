// monster_madness - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>

#include <rex/filesystem.h>
#include <rex/rex_app.h>

class MonsterMadnessApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MonsterMadnessApp>(new MonsterMadnessApp(ctx, "monster_madness",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }

    const auto source_root = std::filesystem::path{MONSTER_MADNESS_SOURCE_ROOT};
    const auto source_assets = source_root / "assets";
    if (std::filesystem::is_directory(source_assets)) {
      paths.game_data_root = source_assets;
      return;
    }

    const auto exe_assets = rex::filesystem::GetExecutableFolder() / "assets";
    if (std::filesystem::is_directory(exe_assets)) {
      paths.game_data_root = exe_assets;
    }
  }

  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = "game:\\patched_default.xex";
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}
};
