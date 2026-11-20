// X-Men: Destiny - ReXGlue recompiled PC port
//
// The program sits in the game folder beside the disc files (Default.xex,
// *.zzz.gz, *.usm, *.bnk ...), so that folder is the game root. Saves, caches
// and logs go in UserData\ there too, which keeps an install self-contained.

#pragma once

#include <rex/rex_app.h>

class XmdApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<XmdApp>(new XmdApp(ctx, "xmd",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostLoadXexImage() override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;

 private:
  void AttachNativeRenderer();
};

