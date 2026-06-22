// monster_madness - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <filesystem>

#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/ui/window.h>

#include "render/mm_graphics.h"
#include "render/video.h"

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

  // Native rendering: install a "null-consumer" graphics system (see
  // mm_graphics.*). Its CommandProcessor drains the ring buffer and services the
  // bookkeeping the guest blocks on (read-pointer write-back, fences, completion
  // interrupts) but performs no rendering, so the game runs frame-by-frame while
  // our D3D hooks take over drawing incrementally. Fully nulling graphics instead
  // hangs the guest after the first swap (nothing consumes the ring).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // MM_MODE_A=1 keeps the SDK's emulated D3D12 renderer (reference output) —
    // used to capture the real game frame under RenderDoc for inspection.
    if (std::getenv("MM_MODE_A")) {
      return;  // leave the default D3D12GraphicsSystem in place
    }
    config.graphics = mm::CreateNullConsumerGraphicsSystem();
  }

  // After the SDK creates the (presenter-less) window, record its native handle so
  // the CP worker thread can create the Plume device/swapchain in SetupContext.
  // (Plume must be inited + used on the CP thread, where IssueDraw/IssueSwap run —
  // not here on the main thread.)
  bool SetupPresentation() override {
    if (!rex::ReXApp::SetupPresentation()) {
      return false;
    }
    if (std::getenv("MM_MODE_A")) {
      return true;  // SDK presenter renders; no Plume.
    }
    if (window()) {
      if (void* hwnd = window()->GetNativeWindowHandle()) {
        mm::Video::Configure(hwnd, 1280, 720);
      }
    }
    return true;
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}
};
