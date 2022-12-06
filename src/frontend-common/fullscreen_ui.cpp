#define IMGUI_DEFINE_MATH_OPERATORS

#include "fullscreen_ui.h"
#include "IconsFontAwesome5.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/make_array.h"
#include "common/string.h"
#include "common/string_util.h"
#include "common_host_interface.h"
#include "controller_interface.h"
#include "core/cheats.h"
#include "core/cpu_core.h"
#include "core/gpu.h"
#include "core/host_display.h"
#include "core/host_interface_progress_callback.h"
#include "core/imgui_fullscreen.h"
#include "core/imgui_styles.h"
#include "core/resources.h"
#include "core/settings.h"
#include "core/system.h"
#include "fullscreen_ui_progress_callback.h"
#include "game_list.h"
#include "icon.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "scmversion/scmversion.h"
#include <bitset>
#include <thread>
Log_SetChannel(FullscreenUI);

#ifdef WITH_CHEEVOS
#include "core/cheevos.h"
#endif

static constexpr float LAYOUT_MAIN_MENU_BAR_SIZE = 20.0f; // Should be DPI scaled, not layout scaled!

using ImGuiFullscreen::g_large_font;
using ImGuiFullscreen::g_layout_padding_left;
using ImGuiFullscreen::g_layout_padding_top;
using ImGuiFullscreen::g_medium_font;
using ImGuiFullscreen::LAYOUT_LARGE_FONT_SIZE;
using ImGuiFullscreen::LAYOUT_MEDIUM_FONT_SIZE;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING;
using ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING;
using ImGuiFullscreen::LAYOUT_SCREEN_HEIGHT;
using ImGuiFullscreen::LAYOUT_SCREEN_WIDTH;

using ImGuiFullscreen::ActiveButton;
using ImGuiFullscreen::BeginFullscreenColumns;
using ImGuiFullscreen::BeginFullscreenColumnWindow;
using ImGuiFullscreen::BeginFullscreenWindow;
using ImGuiFullscreen::BeginMenuButtons;
using ImGuiFullscreen::BeginNavBar;
using ImGuiFullscreen::CloseChoiceDialog;
using ImGuiFullscreen::CloseFileSelector;
using ImGuiFullscreen::DPIScale;
using ImGuiFullscreen::EndFullscreenColumns;
using ImGuiFullscreen::EndFullscreenColumnWindow;
using ImGuiFullscreen::EndFullscreenWindow;
using ImGuiFullscreen::EndMenuButtons;
using ImGuiFullscreen::EndNavBar;
using ImGuiFullscreen::EnumChoiceButton;
using ImGuiFullscreen::FloatingButton;
using ImGuiFullscreen::GetCachedTexture;
using ImGuiFullscreen::LayoutScale;
using ImGuiFullscreen::MenuButton;
using ImGuiFullscreen::MenuButtonFrame;
using ImGuiFullscreen::MenuButtonWithValue;
using ImGuiFullscreen::MenuHeading;
using ImGuiFullscreen::MenuHeadingButton;
using ImGuiFullscreen::MenuImageButton;
using ImGuiFullscreen::NavButton;
using ImGuiFullscreen::NavTitle;
using ImGuiFullscreen::OpenChoiceDialog;
using ImGuiFullscreen::OpenFileSelector;
using ImGuiFullscreen::RangeButton;
using ImGuiFullscreen::RightAlignNavButtons;
using ImGuiFullscreen::ToggleButton;

namespace FullscreenUI {

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////
static void LoadSettings();
static void ClearImGuiFocus();
static void ReturnToMainWindow();
static void DrawLandingWindow();
static void DrawQuickMenu(MainWindowType type);
static void DrawAchievementWindow();
static void DrawLeaderboardsWindow();
static void DrawDebugMenu();
static void DrawAboutWindow();
static void OpenAboutWindow();
static void SetDebugMenuEnabled(bool enabled);
static void UpdateDebugMenuVisibility();

static ALWAYS_INLINE bool IsCheevosHardcoreModeActive()
{
#ifdef WITH_CHEEVOS
  return Cheevos::IsChallengeModeActive();
#else
  return false;
#endif
}

static CommonHostInterface* s_host_interface;
static MainWindowType s_current_main_window = MainWindowType::Landing;
static std::bitset<static_cast<u32>(FrontendCommon::ControllerNavigationButton::Count)> s_nav_input_values{};
static bool s_debug_menu_enabled = false;
static bool s_debug_menu_allowed = false;
static bool s_quick_menu_was_open = false;
static bool s_was_paused_on_quick_menu_open = false;
static bool s_about_window_open = false;
static u32 s_close_button_state = 0;
static std::optional<u32> s_open_leaderboard_id;

//////////////////////////////////////////////////////////////////////////
// Resources
//////////////////////////////////////////////////////////////////////////
static bool LoadResources();
static void DestroyResources();

static std::unique_ptr<HostDisplayTexture> LoadTextureCallback(const char* path);

static std::unique_ptr<HostDisplayTexture> s_app_icon_texture;
static std::unique_ptr<HostDisplayTexture> s_placeholder_texture;
static std::array<std::unique_ptr<HostDisplayTexture>, static_cast<u32>(DiscRegion::Count)> s_disc_region_textures;
static std::array<std::unique_ptr<HostDisplayTexture>, static_cast<u32>(GameListCompatibilityRating::Count)>
  s_game_compatibility_textures;
static std::unique_ptr<HostDisplayTexture> s_fallback_disc_texture;
static std::unique_ptr<HostDisplayTexture> s_fallback_exe_texture;
static std::unique_ptr<HostDisplayTexture> s_fallback_psf_texture;
static std::unique_ptr<HostDisplayTexture> s_fallback_playlist_texture;

//////////////////////////////////////////////////////////////////////////
// Settings
//////////////////////////////////////////////////////////////////////////

enum class InputBindingType
{
  None,
  Button,
  Axis,
  HalfAxis,
  Rumble
};

static constexpr double INPUT_BINDING_TIMEOUT_SECONDS = 5.0;

static void DrawSettingsWindow();
static void BeginInputBinding(InputBindingType type, const std::string_view& section, const std::string_view& key,
                              const std::string_view& display_name);
static void EndInputBinding();
static void ClearInputBinding(const char* section, const char* key);
static void DrawInputBindingWindow();

static SettingsPage s_settings_page = SettingsPage::InterfaceSettings;
static Settings s_settings_copy;
static InputBindingType s_input_binding_type = InputBindingType::None;
static TinyString s_input_binding_section;
static TinyString s_input_binding_key;
static TinyString s_input_binding_display_name;
static bool s_input_binding_keyboard_pressed;
static Common::Timer s_input_binding_timer;

//////////////////////////////////////////////////////////////////////////
// Save State List
//////////////////////////////////////////////////////////////////////////
struct SaveStateListEntry
{
  std::string title;
  std::string summary;
  std::string path;
  std::string media_path;
  std::unique_ptr<HostDisplayTexture> preview_texture;
  s32 slot;
  bool global;
};

static void InitializePlaceholderSaveStateListEntry(SaveStateListEntry* li, s32 slot, bool global);
static void InitializeSaveStateListEntry(SaveStateListEntry* li, CommonHostInterface::ExtendedSaveStateInfo* ssi);
static void PopulateSaveStateListEntries();
static void OpenSaveStateSelector(bool is_loading);
static void CloseSaveStateSelector();
static void DrawSaveStateSelector(bool is_loading, bool fullscreen);

static std::vector<SaveStateListEntry> s_save_state_selector_slots;
static bool s_save_state_selector_open = false;
static bool s_save_state_selector_loading = true;

//////////////////////////////////////////////////////////////////////////
// Game List
//////////////////////////////////////////////////////////////////////////
static void DrawGameListWindow();
static void SwitchToGameList();
static void SortGameList();
static HostDisplayTexture* GetTextureForGameListEntryType(GameListEntryType type);
static HostDisplayTexture* GetGameListCover(const GameListEntry* entry);
static HostDisplayTexture* GetCoverForCurrentGame();

// Lazily populated cover images.
static std::unordered_map<std::string, std::string> s_cover_image_map;
static std::vector<const GameListEntry*> s_game_list_sorted_entries;
static std::thread s_game_list_load_thread;

//////////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////////

bool Initialize(CommonHostInterface* host_interface)
{
  s_host_interface = host_interface;
  if (!ImGuiFullscreen::Initialize() || !LoadResources())
  {
    ImGuiFullscreen::Shutdown();
    return false;
  }

  s_settings_copy.Load(*s_host_interface->GetSettingsInterface());
  LoadSettings();
  UpdateDebugMenuVisibility();

  ImGuiFullscreen::UpdateLayoutScale();
  ImGuiFullscreen::UpdateFonts();
  ImGuiFullscreen::SetLoadTextureFunction(LoadTextureCallback);

  if (System::IsValid())
    SystemCreated();

  return true;
}

bool IsInitialized()
{
  return (s_host_interface != nullptr);
}

bool HasActiveWindow()
{
  return s_current_main_window != MainWindowType::None || s_save_state_selector_open ||
         ImGuiFullscreen::IsChoiceDialogOpen() || ImGuiFullscreen::IsFileSelectorOpen();
}

void LoadSettings() {}

void UpdateSettings()
{
  LoadSettings();
}

void SystemCreated()
{
  s_current_main_window = MainWindowType::None;
  ClearImGuiFocus();
}

void SystemDestroyed()
{
  s_current_main_window = MainWindowType::Landing;
  s_quick_menu_was_open = false;
  ClearImGuiFocus();
}

static void PauseForMenuOpen()
{
  s_was_paused_on_quick_menu_open = System::IsPaused();
  if (s_settings_copy.pause_on_menu && !s_was_paused_on_quick_menu_open)
    s_host_interface->RunLater([]() { s_host_interface->PauseSystem(true); });

  s_quick_menu_was_open = true;
}

void OpenQuickMenu()
{
  if (!System::IsValid() || s_current_main_window != MainWindowType::None)
    return;

  PauseForMenuOpen();

  s_current_main_window = MainWindowType::QuickMenu;
  ClearImGuiFocus();
}

void CloseQuickMenu()
{
  if (!System::IsValid())
    return;

  if (System::IsPaused() && !s_was_paused_on_quick_menu_open)
    s_host_interface->RunLater([]() { s_host_interface->PauseSystem(false); });

  s_current_main_window = MainWindowType::None;
  s_quick_menu_was_open = false;
  ClearImGuiFocus();
}

#ifdef WITH_CHEEVOS

bool OpenAchievementsWindow()
{
  const bool achievements_enabled = Cheevos::HasActiveGame() && (Cheevos::GetAchievementCount() > 0);
  if (!achievements_enabled)
    return false;

  if (!s_quick_menu_was_open)
    PauseForMenuOpen();

  s_current_main_window = MainWindowType::Achievements;
  return true;
}

bool OpenLeaderboardsWindow()
{
  const bool leaderboards_enabled = Cheevos::HasActiveGame() && (Cheevos::GetLeaderboardCount() > 0);
  if (!leaderboards_enabled)
    return false;

  if (!s_quick_menu_was_open)
    PauseForMenuOpen();

  s_current_main_window = MainWindowType::Leaderboards;
  s_open_leaderboard_id.reset();
  return true;
}

#endif

void Shutdown()
{
  if (s_game_list_load_thread.joinable())
    s_game_list_load_thread.join();

  CloseSaveStateSelector();
  s_cover_image_map.clear();
  s_nav_input_values = {};
  DestroyResources();
  ImGuiFullscreen::Shutdown();

  s_host_interface = nullptr;
}

void Render()
{
  if (s_debug_menu_enabled)
    DrawDebugMenu();

  if (System::IsValid())
  {
    if (!s_debug_menu_enabled)
      s_host_interface->DrawStatsOverlay();

    if (!IsCheevosHardcoreModeActive())
      s_host_interface->DrawDebugWindows();
  }

  ImGuiFullscreen::BeginLayout();

  switch (s_current_main_window)
  {
    case MainWindowType::Landing:
      DrawLandingWindow();
      break;
    case MainWindowType::GameList:
      DrawGameListWindow();
      break;
    case MainWindowType::Settings:
      DrawSettingsWindow();
      break;
    case MainWindowType::QuickMenu:
      DrawQuickMenu(s_current_main_window);
      break;
    case MainWindowType::Achievements:
      DrawAchievementWindow();
      break;
    case MainWindowType::Leaderboards:
      DrawLeaderboardsWindow();
      break;
    default:
      break;
  }

  if (s_save_state_selector_open)
    DrawSaveStateSelector(s_save_state_selector_loading, false);

  if (s_about_window_open)
    DrawAboutWindow();

  if (s_input_binding_type != InputBindingType::None)
    DrawInputBindingWindow();

  s_host_interface->DrawOSDMessages();

  ImGuiFullscreen::EndLayout();
}

Settings& GetSettingsCopy()
{
  return s_settings_copy;
}

void SaveAndApplySettings()
{
  s_settings_copy.Save(*s_host_interface->GetSettingsInterface());
  s_host_interface->GetSettingsInterface()->Save();
  s_host_interface->ApplySettings(false);
  UpdateSettings();
}

void ClearImGuiFocus()
{
  ImGui::SetWindowFocus(nullptr);
  s_close_button_state = 0;
}

void ReturnToMainWindow()
{
  if (s_quick_menu_was_open)
    CloseQuickMenu();

  s_current_main_window = System::IsValid() ? MainWindowType::None : MainWindowType::Landing;
}

bool LoadResources()
{
  if (!(s_app_icon_texture = LoadTextureResource("logo.png", false)) &&
      !(s_app_icon_texture = LoadTextureResource("duck.png")))
  {
    return false;
  }

  if (!(s_placeholder_texture = s_host_interface->GetDisplay()->CreateTexture(
          PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1, HostDisplayPixelFormat::RGBA8,
          PLACEHOLDER_ICON_DATA, sizeof(u32) * PLACEHOLDER_ICON_WIDTH, false)))
  {
    return false;
  }

  if (!(s_disc_region_textures[static_cast<u32>(DiscRegion::NTSC_U)] = LoadTextureResource("flag-uc.png")) ||
      !(s_disc_region_textures[static_cast<u32>(DiscRegion::NTSC_J)] = LoadTextureResource("flag-jp.png")) ||
      !(s_disc_region_textures[static_cast<u32>(DiscRegion::PAL)] = LoadTextureResource("flag-eu.png")) ||
      !(s_disc_region_textures[static_cast<u32>(DiscRegion::Other)] = LoadTextureResource("flag-eu.png")) ||
      !(s_fallback_disc_texture = LoadTextureResource("media-cdrom.png")) ||
      !(s_fallback_exe_texture = LoadTextureResource("applications-system.png")) ||
      !(s_fallback_psf_texture = LoadTextureResource("multimedia-player.png")) ||
      !(s_fallback_playlist_texture = LoadTextureResource("address-book-new.png")))
  {
    return false;
  }

  for (u32 i = 0; i < static_cast<u32>(GameListCompatibilityRating::Count); i++)
  {
    if (!(s_game_compatibility_textures[i] = LoadTextureResource(TinyString::FromFormat("star-%u.png", i))))
      return false;
  }

  return true;
}

void DestroyResources()
{
  s_app_icon_texture.reset();
  s_placeholder_texture.reset();
  s_fallback_playlist_texture.reset();
  s_fallback_psf_texture.reset();
  s_fallback_exe_texture.reset();
  s_fallback_disc_texture.reset();
  for (auto& tex : s_game_compatibility_textures)
    tex.reset();
  for (auto& tex : s_disc_region_textures)
    tex.reset();
}

static std::unique_ptr<HostDisplayTexture> LoadTexture(const char* path, bool from_package)
{
  std::unique_ptr<ByteStream> stream;
  if (from_package)
    stream = g_host_interface->OpenPackageFile(path, BYTESTREAM_OPEN_READ);
  else
    stream = FileSystem::OpenFile(path, BYTESTREAM_OPEN_READ);
  if (!stream)
  {
    Log_ErrorPrintf("Failed to open texture resource '%s'", path);
    return {};
  }

  Common::RGBA8Image image;
  if (!Common::LoadImageFromStream(&image, stream.get()) && image.IsValid())
  {
    Log_ErrorPrintf("Failed to read texture resource '%s'", path);
    return {};
  }

  std::unique_ptr<HostDisplayTexture> texture = g_host_interface->GetDisplay()->CreateTexture(
    image.GetWidth(), image.GetHeight(), 1, 1, 1, HostDisplayPixelFormat::RGBA8, image.GetPixels(),
    image.GetByteStride());
  if (!texture)
  {
    Log_ErrorPrintf("failed to create %ux%u texture for resource", image.GetWidth(), image.GetHeight());
    return {};
  }

  Log_DevPrintf("Uploaded texture resource '%s' (%ux%u)", path, image.GetWidth(), image.GetHeight());
  return texture;
}

std::unique_ptr<HostDisplayTexture> LoadTextureCallback(const char* path)
{
  return LoadTexture(path, false);
}

std::unique_ptr<HostDisplayTexture> LoadTextureResource(const char* name, bool allow_fallback /*= true*/)
{
  const std::string path(StringUtil::StdStringFromFormat("resources" FS_OSPATH_SEPARATOR_STR "%s", name));
  std::unique_ptr<HostDisplayTexture> texture = LoadTexture(path.c_str(), true);
  if (texture)
    return texture;

  if (!allow_fallback)
    return nullptr;

  Log_ErrorPrintf("Missing resource '%s', using fallback", name);

  texture = g_host_interface->GetDisplay()->CreateTexture(PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1,
                                                          HostDisplayPixelFormat::RGBA8, PLACEHOLDER_ICON_DATA,
                                                          sizeof(u32) * PLACEHOLDER_ICON_WIDTH, false);
  if (!texture)
    Panic("Failed to create placeholder texture");

  return texture;
}

//////////////////////////////////////////////////////////////////////////
// Utility
//////////////////////////////////////////////////////////////////////////

static ImGuiFullscreen::FileSelectorFilters GetDiscImageFilters()
{
  return {"*.bin",   "*.cue", "*.iso", "*.img",     "*.chd", "*.ecm", "*.mds",
          "*.psexe", "*.exe", "*.psf", "*.minipsf", "*.m3u", "*.pbp"};
}

static void DoStartPath(const std::string& path, bool allow_resume)
{
  // we can never resume from exe/psf, or when challenge mode is active
  if (System::IsExeFileName(path.c_str()) || System::IsPsfFileName(path.c_str()) ||
      s_host_interface->IsCheevosChallengeModeActive())
    allow_resume = false;

  if (allow_resume && g_settings.save_state_on_exit)
  {
    s_host_interface->RunLater([path]() { s_host_interface->ResumeSystemFromState(path.c_str(), true); });
  }
  else
  {
    auto params = std::make_shared<SystemBootParameters>(path);
    s_host_interface->RunLater([params]() { s_host_interface->BootSystem(std::move(params)); });
  }
}

static void DoStartFile()
{
  auto callback = [](const std::string& path) {
    if (!path.empty())
      DoStartPath(path, false);

    ClearImGuiFocus();
    CloseFileSelector();
  };

  OpenFileSelector(ICON_FA_COMPACT_DISC "  Select Disc Image", false, std::move(callback), GetDiscImageFilters());
}

static void DoStartBIOS()
{
  s_host_interface->RunLater([]() { s_host_interface->BootSystem(std::make_shared<SystemBootParameters>()); });
  ClearImGuiFocus();
}

static void DoPowerOff()
{
  s_host_interface->RunLater([]() {
    if (!System::IsValid())
      return;

    s_host_interface->PowerOffSystem(s_host_interface->ShouldSaveResumeState());

    ReturnToMainWindow();
  });
  ClearImGuiFocus();
}

static void DoReset()
{
  s_host_interface->RunLater([]() {
    if (!System::IsValid())
      return;

    s_host_interface->ResetSystem();
  });
}

static void DoPause()
{
  s_host_interface->RunLater([]() {
    if (!System::IsValid())
      return;

    s_host_interface->PauseSystem(!System::IsPaused());
  });
}

static void DoCheatsMenu()
{
  CheatList* cl = System::GetCheatList();
  if (!cl)
  {
    if (!s_host_interface->LoadCheatListFromDatabase() || ((cl = System::GetCheatList()) == nullptr))
    {
      s_host_interface->AddFormattedOSDMessage(10.0f, "No cheats found for %s.", System::GetRunningTitle().c_str());
      ReturnToMainWindow();
      return;
    }
  }

  ImGuiFullscreen::ChoiceDialogOptions options;
  options.reserve(cl->GetCodeCount());
  for (u32 i = 0; i < cl->GetCodeCount(); i++)
  {
    const CheatCode& cc = cl->GetCode(i);
    options.emplace_back(cc.description.c_str(), cc.enabled);
  }

  auto callback = [](s32 index, const std::string& title, bool checked) {
    if (index < 0)
    {
      ReturnToMainWindow();
      return;
    }

    CheatList* cl = System::GetCheatList();
    if (!cl)
      return;

    const CheatCode& cc = cl->GetCode(static_cast<u32>(index));
    if (cc.activation == CheatCode::Activation::Manual)
      cl->ApplyCode(static_cast<u32>(index));
    else
      s_host_interface->SetCheatCodeState(static_cast<u32>(index), checked, true);
  };
  OpenChoiceDialog(ICON_FA_FROWN "  Cheat List", true, std::move(options), std::move(callback));
}

static void DoToggleAnalogMode()
{
  // hacky way to toggle analog mode
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    Controller* ctrl = System::GetController(i);
    if (!ctrl)
      continue;

    std::optional<s32> code = Controller::GetButtonCodeByName(ctrl->GetType(), "Analog");
    if (!code.has_value())
      continue;

    ctrl->SetButtonState(code.value(), true);
    ctrl->SetButtonState(code.value(), false);
  }
}

static void DoChangeDiscFromFile()
{
  auto callback = [](const std::string& path) {
    if (!path.empty())
      System::InsertMedia(path.c_str());

    ClearImGuiFocus();
    CloseFileSelector();
    ReturnToMainWindow();
  };

  OpenFileSelector(ICON_FA_COMPACT_DISC "  Select Disc Image", false, std::move(callback), GetDiscImageFilters(),
                   std::string(FileSystem::GetPathDirectory(System::GetMediaFileName())));
}

static void DoChangeDisc()
{
  if (!System::HasMediaSubImages())
  {
    DoChangeDiscFromFile();
    return;
  }

  const u32 current_index = System::GetMediaSubImageIndex();
  const u32 count = System::GetMediaSubImageCount();
  ImGuiFullscreen::ChoiceDialogOptions options;
  options.reserve(count + 1);
  options.emplace_back("From File...", false);

  for (u32 i = 0; i < count; i++)
    options.emplace_back(System::GetMediaSubImageTitle(i), i == current_index);

  auto callback = [](s32 index, const std::string& title, bool checked) {
    if (index == 0)
    {
      CloseChoiceDialog();
      DoChangeDiscFromFile();
      return;
    }
    else if (index > 0)
    {
      System::SwitchMediaSubImage(static_cast<u32>(index - 1));
    }

    ClearImGuiFocus();
    CloseChoiceDialog();
    ReturnToMainWindow();
  };

  OpenChoiceDialog(ICON_FA_COMPACT_DISC "  Select Disc Image", true, std::move(options), std::move(callback));
}

//////////////////////////////////////////////////////////////////////////
// Landing Window
//////////////////////////////////////////////////////////////////////////

void DrawLandingWindow()
{
  BeginFullscreenColumns();

  if (BeginFullscreenColumnWindow(0.0f, 570.0f, "logo", ImVec4(0.11f, 0.15f, 0.17f, 1.00f)))
  {
    const float image_size = LayoutScale(380.f);
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() * 0.5f) - (image_size * 0.5f),
                               (ImGui::GetWindowHeight() * 0.5f) - (image_size * 0.5f)));
    ImGui::Image(s_app_icon_texture->GetHandle(), ImVec2(image_size, image_size));
  }
  EndFullscreenColumnWindow();

  if (BeginFullscreenColumnWindow(570.0f, LAYOUT_SCREEN_WIDTH, "menu"))
  {
    BeginMenuButtons(7, 0.5f);

    if (MenuButton(" " ICON_FA_PLAY_CIRCLE "  Resume",
                   "Starts the console from where it was before it was last closed.", !IsCheevosHardcoreModeActive()))
    {
      s_host_interface->RunLater([]() { s_host_interface->ResumeSystemFromMostRecentState(); });
      ClearImGuiFocus();
    }

    if (MenuButton(" " ICON_FA_LIST "  Open Game List",
                   "Launch a game from images scanned from your game directories."))
    {
      s_host_interface->RunLater(SwitchToGameList);
    }

    if (MenuButton(" " ICON_FA_FOLDER_OPEN "  Start File", "Launch a game by selecting a file/disc image."))
      s_host_interface->RunLater(DoStartFile);

    if (MenuButton(" " ICON_FA_TOOLBOX "  Start BIOS", "Start the console without any disc inserted."))
      s_host_interface->RunLater(DoStartBIOS);

    if (MenuButton(" " ICON_FA_UNDO "  Load State", "Loads a global save state.", !IsCheevosHardcoreModeActive()))
    {
      OpenSaveStateSelector(true);
    }

    if (MenuButton(" " ICON_FA_SLIDERS_H "  Settings", "Change settings for the emulator."))
      s_current_main_window = MainWindowType::Settings;

    if (MenuButton(" " ICON_FA_SIGN_OUT_ALT "  Exit", "Exits the program."))
      s_host_interface->RequestExit();

    {
      ImVec2 fullscreen_pos;
      if (FloatingButton(ICON_FA_WINDOW_CLOSE, 0.0f, 0.0f, -1.0f, -1.0f, 1.0f, 0.0f, true, g_large_font,
                         &fullscreen_pos))
      {
        s_host_interface->RequestExit();
      }

      if (FloatingButton(ICON_FA_EXPAND, fullscreen_pos.x, 0.0f, -1.0f, -1.0f, -1.0f, 0.0f, true, g_large_font,
                         &fullscreen_pos))
      {
        s_host_interface->RunLater([]() { s_host_interface->SetFullscreen(!s_host_interface->IsFullscreen()); });
      }

      if (FloatingButton(ICON_FA_QUESTION_CIRCLE, fullscreen_pos.x, 0.0f, -1.0f, -1.0f, -1.0f, 0.0f))
        OpenAboutWindow();
    }

    EndMenuButtons();
  }

  EndFullscreenColumnWindow();

  EndFullscreenColumns();
}

static ImGuiFullscreen::ChoiceDialogOptions GetGameListDirectoryOptions(bool recursive_as_checked)
{
  ImGuiFullscreen::ChoiceDialogOptions options;

  for (std::string& dir : s_host_interface->GetSettingsInterface()->GetStringList("GameList", "Paths"))
    options.emplace_back(std::move(dir), false);

  for (std::string& dir : s_host_interface->GetSettingsInterface()->GetStringList("GameList", "RecursivePaths"))
    options.emplace_back(std::move(dir), recursive_as_checked);

  std::sort(options.begin(), options.end(), [](const auto& lhs, const auto& rhs) {
    return (StringUtil::Strcasecmp(lhs.first.c_str(), rhs.first.c_str()) < 0);
  });

  return options;
}

static void DrawInputBindingButton(InputBindingType type, const char* section, const char* name,
                                   const char* display_name, bool show_type = true)
{
  TinyString title;
  title.Format("%s/%s", section, name);

  ImRect bb;
  bool visible, hovered, clicked;
  clicked =
    MenuButtonFrame(title, true, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT, &visible, &hovered, &bb.Min, &bb.Max);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
  const ImRect title_bb(bb.Min, ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(bb.Min.x, midpoint), bb.Max);

  if (show_type)
  {
    switch (type)
    {
      case InputBindingType::Button:
        title.Format(ICON_FA_CIRCLE "  %s Button", display_name);
        break;
      case InputBindingType::Axis:
        title.Format(ICON_FA_BULLSEYE "  %s Axis", display_name);
        break;
      case InputBindingType::HalfAxis:
        title.Format(ICON_FA_SLIDERS_H "  %s Half-Axis", display_name);
        break;
      case InputBindingType::Rumble:
        title.Format(ICON_FA_BELL "  %s", display_name);
        break;
      default:
        title = display_name;
        break;
    }
  }

  ImGui::PushFont(g_large_font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, show_type ? title.GetCharArray() : display_name, nullptr,
                           nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  // eek, potential heap allocation :/
  const std::string value = s_host_interface->GetSettingsInterface()->GetStringValue(section, name);
  ImGui::PushFont(g_medium_font);
  ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, value.empty() ? "(No Binding)" : value.c_str(), nullptr,
                           nullptr, ImVec2(0.0f, 0.0f), &summary_bb);
  ImGui::PopFont();

  if (clicked)
    BeginInputBinding(type, section, name, display_name);
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    ClearInputBinding(section, name);
}

static void ClearInputBindingVariables()
{
  s_input_binding_type = InputBindingType::None;
  s_input_binding_section.Clear();
  s_input_binding_key.Clear();
  s_input_binding_display_name.Clear();
}

bool IsBindingInput()
{
  return s_input_binding_type != InputBindingType::None;
}

bool HandleKeyboardBinding(const char* keyName, bool pressed)
{
  if (s_input_binding_type == InputBindingType::None)
    return false;

  if (pressed)
  {
    s_input_binding_keyboard_pressed = true;
    return true;
  }

  if (!s_input_binding_keyboard_pressed)
    return false;

  TinyString value;
  value.Format("Keyboard/%s", keyName);

  {
    auto lock = s_host_interface->GetSettingsLock();
    s_host_interface->GetSettingsInterface()->SetStringValue(s_input_binding_section, s_input_binding_key, value);
  }

  EndInputBinding();
  s_host_interface->RunLater(SaveAndApplySettings);
  return true;
}

void BeginInputBinding(InputBindingType type, const std::string_view& section, const std::string_view& key,
                       const std::string_view& display_name)
{
  s_input_binding_type = type;
  s_input_binding_section = section;
  s_input_binding_key = key;
  s_input_binding_display_name = display_name;
  s_input_binding_timer.Reset();

  ControllerInterface* ci = s_host_interface->GetControllerInterface();
  if (ci)
  {
    auto callback = [](const ControllerInterface::Hook& hook) -> ControllerInterface::Hook::CallbackResult {
      // ignore if axis isn't at least halfway
      if (hook.type == ControllerInterface::Hook::Type::Axis && std::abs(std::get<float>(hook.value)) < 0.5f)
        return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;

      TinyString value;
      switch (s_input_binding_type)
      {
        case InputBindingType::Axis:
        {
          if (hook.type == ControllerInterface::Hook::Type::Axis)
            value.Format("Controller%d/Axis%d", hook.controller_index, hook.button_or_axis_number);
        }
        break;

        case InputBindingType::HalfAxis:
        {
          if (hook.type == ControllerInterface::Hook::Type::Axis)
          {
            value.Format("Controller%d/%cAxis%d", hook.controller_index,
                         (std::get<float>(hook.value) < 0.0f) ? '-' : '+', hook.button_or_axis_number);
          }
        }
        break;

        case InputBindingType::Button:
        {
          if (hook.type == ControllerInterface::Hook::Type::Axis)
            value.Format("Controller%d/+Axis%d", hook.controller_index, hook.button_or_axis_number);
          else if (hook.type == ControllerInterface::Hook::Type::Button && std::get<float>(hook.value) > 0.0f)
            value.Format("Controller%d/Button%d", hook.controller_index, hook.button_or_axis_number);
        }
        break;

        case InputBindingType::Rumble:
        {
          if (hook.type == ControllerInterface::Hook::Type::Button && std::get<float>(hook.value) > 0.0f)
            value.Format("Controller%d", hook.controller_index);
        }
        break;

        default:
          break;
      }

      if (value.IsEmpty())
        return ControllerInterface::Hook::CallbackResult::ContinueMonitoring;

      {
        auto lock = s_host_interface->GetSettingsLock();
        s_host_interface->GetSettingsInterface()->SetStringValue(s_input_binding_section, s_input_binding_key, value);
      }

      ClearInputBindingVariables();
      s_host_interface->RunLater(SaveAndApplySettings);

      return ControllerInterface::Hook::CallbackResult::StopMonitoring;
    };
    ci->SetHook(std::move(callback));
  }
}

void EndInputBinding()
{
  ClearInputBindingVariables();

  ControllerInterface* ci = s_host_interface->GetControllerInterface();
  if (ci)
    ci->ClearHook();
}

void ClearInputBinding(const char* section, const char* key)
{
  {
    auto lock = s_host_interface->GetSettingsLock();
    s_host_interface->GetSettingsInterface()->DeleteValue(section, key);
  }

  s_host_interface->RunLater(SaveAndApplySettings);
}

void DrawInputBindingWindow()
{
  DebugAssert(s_input_binding_type != InputBindingType::None);

  const double time_remaining = INPUT_BINDING_TIMEOUT_SECONDS - s_input_binding_timer.GetTimeSeconds();
  if (time_remaining <= 0.0)
  {
    EndInputBinding();
    return;
  }

  const char* title = ICON_FA_GAMEPAD "  Set Input Binding";
  ImGui::SetNextWindowSize(LayoutScale(500.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup(title);

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                              ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(10.0f, 10.0f));

  if (ImGui::BeginPopupModal(title, nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
  {
    ImGui::TextWrapped("Setting %s binding %s.", s_input_binding_section.GetCharArray(),
                       s_input_binding_display_name.GetCharArray());
    ImGui::TextUnformatted("Push a controller button or axis now.");
    ImGui::NewLine();
    ImGui::Text("Timing out in %.0f seconds...", time_remaining);
    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(3);
  ImGui::PopFont();
}

static bool SettingInfoButton(const SettingInfo& si, const char* section)
{
  // this.. isn't pretty :(
  TinyString title;
  title.Format("%s##%s/%s", si.visible_name, section, si.key);
  switch (si.type)
  {
    case SettingInfo::Type::Boolean:
    {
      bool value = s_host_interface->GetSettingsInterface()->GetBoolValue(
        section, si.key, StringUtil::FromChars<bool>(si.default_value).value_or(false));
      if (ToggleButton(title, si.description, &value))
      {
        s_host_interface->GetSettingsInterface()->SetBoolValue(section, si.key, value);
        return true;
      }

      return false;
    }

    case SettingInfo::Type::Integer:
    {
      int value = s_host_interface->GetSettingsInterface()->GetIntValue(
        section, si.key, StringUtil::FromChars<int>(si.default_value).value_or(0));
      const int min = StringUtil::FromChars<int>(si.min_value).value_or(0);
      const int max = StringUtil::FromChars<int>(si.max_value).value_or(0);
      const int step = StringUtil::FromChars<int>(si.step_value).value_or(0);
      if (RangeButton(title, si.description, &value, min, max, step))
      {
        s_host_interface->GetSettingsInterface()->SetIntValue(section, si.key, value);
        return true;
      }

      return false;
    }

    case SettingInfo::Type::Float:
    {
      float value = s_host_interface->GetSettingsInterface()->GetFloatValue(
        section, si.key, StringUtil::FromChars<float>(si.default_value).value_or(0));
      const float min = StringUtil::FromChars<float>(si.min_value).value_or(0);
      const float max = StringUtil::FromChars<float>(si.max_value).value_or(0);
      const float step = StringUtil::FromChars<float>(si.step_value).value_or(0);
      if (RangeButton(title, si.description, &value, min, max, step))
      {
        s_host_interface->GetSettingsInterface()->SetFloatValue(section, si.key, value);
        return true;
      }

      return false;
    }

    case SettingInfo::Type::Path:
    {
      std::string value = s_host_interface->GetSettingsInterface()->GetStringValue(section, si.key);
      if (MenuButtonWithValue(title, si.description, value.c_str()))
      {
        std::string section_copy(section);
        std::string key_copy(si.key);
        auto callback = [section_copy, key_copy](const std::string& path) {
          if (!path.empty())
          {
            s_host_interface->GetSettingsInterface()->SetStringValue(section_copy.c_str(), key_copy.c_str(),
                                                                     path.c_str());
            s_host_interface->RunLater(SaveAndApplySettings);
          }

          ClearImGuiFocus();
          CloseFileSelector();
        };
        OpenFileSelector(si.visible_name, false, std::move(callback), ImGuiFullscreen::FileSelectorFilters(),
                         std::string(FileSystem::GetPathDirectory(std::move(value))));
      }

      return false;
    }

    default:
      return false;
  }
}

static bool ToggleButtonForNonSetting(const char* title, const char* summary, const char* section, const char* key,
                                      bool default_value, bool enabled = true,
                                      float height = ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT,
                                      ImFont* font = g_large_font, ImFont* summary_font = g_medium_font)
{
  bool value = s_host_interface->GetSettingsInterface()->GetBoolValue(section, key, default_value);
  if (!ToggleButton(title, summary, &value, enabled, height, font, summary_font))
    return false;

  s_host_interface->GetSettingsInterface()->SetBoolValue(section, key, value);
  return true;
}

#ifdef WITH_CHEEVOS

static void DrawAchievementsLoginWindow()
{
  ImGui::SetNextWindowSize(LayoutScale(700.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));
  ImGui::PushFont(g_large_font);

  bool is_open = true;
  if (ImGui::BeginPopupModal("Achievements Login", &is_open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {

    ImGui::TextWrapped("Please enter user name and password for retroachievements.org.");
    ImGui::NewLine();
    ImGui::TextWrapped(
      "Your password will not be saved in DuckStation, an access token will be generated and used instead.");

    ImGui::NewLine();

    static char username[256] = {};
    static char password[256] = {};

    ImGui::Text("User Name: ");
    ImGui::SameLine(LayoutScale(200.0f));
    ImGui::InputText("##username", username, sizeof(username));

    ImGui::Text("Password: ");
    ImGui::SameLine(LayoutScale(200.0f));
    ImGui::InputText("##password", password, sizeof(password), ImGuiInputTextFlags_Password);

    ImGui::NewLine();

    BeginMenuButtons();

    const bool login_enabled = (std::strlen(username) > 0 && std::strlen(password) > 0);

    if (ActiveButton(ICON_FA_KEY "  Login", false, login_enabled))
    {
      Cheevos::LoginAsync(username, password);
      std::memset(username, 0, sizeof(username));
      std::memset(password, 0, sizeof(password));
      ImGui::CloseCurrentPopup();
    }

    if (ActiveButton(ICON_FA_TIMES "  Cancel", false))
      ImGui::CloseCurrentPopup();

    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopFont();
  ImGui::PopStyleVar(2);
}

static bool ConfirmChallengeModeEnable()
{
  if (!System::IsValid())
    return true;

  const bool cheevos_enabled = s_host_interface->GetBoolSettingValue("Cheevos", "Enabled", false);
  const bool cheevos_hardcore = s_host_interface->GetBoolSettingValue("Cheevos", "ChallengeMode", false);
  if (!cheevos_enabled || !cheevos_hardcore)
    return true;

  SmallString message;
  message.AppendString("Enabling hardcore mode will shut down your current game.\n\n");

  if (s_host_interface->ShouldSaveResumeState())
  {
    message.AppendString(
      "The current state will be saved, but you will be unable to load it until you disable hardcore mode.\n\n");
  }

  message.AppendString("Do you want to continue?");

  if (!s_host_interface->ConfirmMessage(message))
    return false;

  SaveAndApplySettings();
  s_host_interface->PowerOffSystem(s_host_interface->ShouldSaveResumeState());
  return true;
}

#endif

static bool WantsToCloseMenu()
{
  // Wait for the Close button to be released, THEN pressed
  if (s_close_button_state == 0)
  {
    if (!ImGuiFullscreen::IsCancelButtonPressed())
      s_close_button_state = 1;
  }
  else if (s_close_button_state == 1)
  {
    if (ImGuiFullscreen::IsCancelButtonPressed())
    {
      s_close_button_state = 0;
      return true;
    }
  }
  return false;
}

void DrawSettingsWindow()
{
  ImGuiIO& io = ImGui::GetIO();
  ImVec2 heading_size = ImVec2(
    io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY + LAYOUT_MENU_BUTTON_Y_PADDING * 2.0f + 2.0f));

  if (BeginFullscreenWindow(ImVec2(0.0f, ImGuiFullscreen::g_menu_bar_size), heading_size, "settings_category",
                            ImVec4(0.18f, 0.18f, 0.18f, 1.00f)))
  {
    static constexpr float ITEM_WIDTH = 22.0f;

    static constexpr std::array<const char*, static_cast<u32>(SettingsPage::Count)> icons = {
      {ICON_FA_WINDOW_MAXIMIZE, ICON_FA_LIST, ICON_FA_HDD, ICON_FA_SLIDERS_H, ICON_FA_MICROCHIP, ICON_FA_GAMEPAD,
       ICON_FA_KEYBOARD, ICON_FA_SD_CARD, ICON_FA_TV, ICON_FA_MAGIC, ICON_FA_HEADPHONES, ICON_FA_TROPHY,
       ICON_FA_EXCLAMATION_TRIANGLE}};

    static constexpr std::array<const char*, static_cast<u32>(SettingsPage::Count)> titles = {
      {"界面设置(极熊座专属汉化)", "游戏列表设置", "机台设置", "模拟器设置", "BIOS设置",
       "控制器设置", "热键设置", "内存卡设置", "显示设置", "增强设置",
       "音频设置", "成就设置", "高级设置"}};

    BeginNavBar();

    if (ImGui::IsNavInputTest(ImGuiNavInput_FocusPrev, ImGuiInputReadMode_Pressed))
    {
      s_settings_page = static_cast<SettingsPage>((s_settings_page == static_cast<SettingsPage>(0)) ?
                                                    (static_cast<u32>(SettingsPage::Count) - 1) :
                                                    (static_cast<u32>(s_settings_page) - 1));
    }
    else if (ImGui::IsNavInputTest(ImGuiNavInput_FocusNext, ImGuiInputReadMode_Pressed))
    {
      s_settings_page =
        static_cast<SettingsPage>((static_cast<u32>(s_settings_page) + 1) % static_cast<u32>(SettingsPage::Count));
    }

    if (NavButton(ICON_FA_BACKWARD, false, true))
      ReturnToMainWindow();

    NavTitle(titles[static_cast<u32>(s_settings_page)]);

    RightAlignNavButtons(static_cast<u32>(titles.size()), ITEM_WIDTH, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    for (u32 i = 0; i < static_cast<u32>(titles.size()); i++)
    {
      if (NavButton(icons[i], s_settings_page == static_cast<SettingsPage>(i), true, ITEM_WIDTH,
                    LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
      {
        s_settings_page = static_cast<SettingsPage>(i);
      }
    }

    EndNavBar();
  }

  EndFullscreenWindow();

  if (BeginFullscreenWindow(
        ImVec2(0.0f, ImGuiFullscreen::g_menu_bar_size + heading_size.y),
        ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y - ImGuiFullscreen::g_menu_bar_size),
        "settings_parent"))
  {
    if (ImGui::IsNavInputTest(ImGuiNavInput_Cancel, ImGuiInputReadMode_Pressed))
    {
      if (ImGui::IsWindowFocused())
        ReturnToMainWindow();
    }

    bool settings_changed = false;

    switch (s_settings_page)
    {
      case SettingsPage::InterfaceSettings:
      {
        BeginMenuButtons();

        MenuHeading("行为");

        settings_changed |=
          ToggleButton("启动时暂停", "游戏启动时暂停模拟器.", &s_settings_copy.start_paused);
        settings_changed |= ToggleButton("焦点丢失时暂停",
                                         "最小化窗口或切换到另一个窗口时暂停模拟器 "
                                         "应用程序，并在切换回时取消暂停。",
                                         &s_settings_copy.pause_on_focus_loss);
        settings_changed |= ToggleButton(
          "暂停菜单", "打开快速菜单时暂停模拟器，关闭时取消暂停.",
          &s_settings_copy.pause_on_menu);
        settings_changed |=
          ToggleButton("确认电源关闭",
                       "确定是否显示提示以确认关闭模拟器/游戏 "
                       "当按下热键时.",
                       &s_settings_copy.confim_power_off);
        settings_changed |=
          ToggleButton("退出时保存状态",
                       "关机或退出时自动保存模拟器状态。你可以"
                       "下次直接从你离开的地方继续.",
                       &s_settings_copy.save_state_on_exit);
        settings_changed |=
          ToggleButton("开始全屏显示", "程序启动时自动切换到全屏模式。",
                       &s_settings_copy.start_fullscreen);
        settings_changed |= ToggleButtonForNonSetting(
          "在全屏模式下隐藏光标“，”当模拟器处于全屏模式时隐藏鼠标指针/光标.",
          "Main", "HideCursorInFullscreen", true);
        settings_changed |= ToggleButton(
          "禁用屏幕保护程序",
          "防止在模拟运行时激活屏幕保护程序和主机休眠.",
          &s_settings_copy.inhibit_screensaver);
        settings_changed |=
          ToggleButton("从保存状态加载设备",
                       "启用后，存储卡和控制器将在加载保存状态时被覆盖.",
                       &s_settings_copy.load_devices_from_save_states);
        settings_changed |= ToggleButton(
          "应用每游戏设置",
          "启用后，将应用每个游戏的设置，并禁用不兼容的增强功能.",
          &s_settings_copy.apply_game_settings);
        settings_changed |=
          ToggleButton("自动加载作弊", "游戏开始时自动加载并应用作弊.",
                       &s_settings_copy.auto_load_cheats);

#ifdef WITH_DISCORD_PRESENCE
        MenuHeading("集成");
        settings_changed |= ToggleButtonForNonSetting(
          "启用Discord Presence", "显示你正在玩的游戏，作为你在Discord上的个人资料的一部分.",
          "Main", "EnableDiscordPresence", false);
#endif

        MenuHeading("杂项");

        static ControllerInterface::Backend cbtype = ControllerInterface::Backend::None;
        static bool cbtype_set = false;
        if (!cbtype_set)
        {
          cbtype = ControllerInterface::ParseBackendName(
                     s_host_interface->GetSettingsInterface()->GetStringValue("Main", "ControllerBackend").c_str())
                     .value_or(ControllerInterface::GetDefaultBackend());
          cbtype_set = true;
        }

        if (EnumChoiceButton("控制器后端", "设置用于接收控制器输入的API。", &cbtype,
                             ControllerInterface::GetBackendName, ControllerInterface::Backend::Count))
        {
          s_host_interface->GetSettingsInterface()->SetStringValue("Main", "ControllerBackend",
                                                                   ControllerInterface::GetBackendName(cbtype));
          settings_changed = true;
        }

        EndMenuButtons();
      }
      break;

      case SettingsPage::GameListSettings:
      {
        BeginMenuButtons();

        MenuHeading("游戏列表");

        if (MenuButton(ICON_FA_FOLDER_PLUS "  添加搜索目录", "将新目录添加到游戏搜索列表."))
        {
          OpenFileSelector(ICON_FA_FOLDER_PLUS "  添加搜索目录", true, [](const std::string& dir) {
            if (!dir.empty())
            {
              s_host_interface->GetSettingsInterface()->AddToStringList("GameList", "RecursivePaths", dir.c_str());
              s_host_interface->GetSettingsInterface()->RemoveFromStringList("GameList", "Paths", dir.c_str());
              s_host_interface->GetSettingsInterface()->Save();
              QueueGameListRefresh();
            }

            CloseFileSelector();
          });
        }

        if (MenuButton(ICON_FA_FOLDER_OPEN "  更改递归目录",
                       "设置是否为每个游戏目录搜索子目录"))
        {
          OpenChoiceDialog(
            ICON_FA_FOLDER_OPEN "  更改递归目录", true, GetGameListDirectoryOptions(true),
            [](s32 index, const std::string& title, bool checked) {
              if (index < 0)
                return;

              if (checked)
              {
                s_host_interface->GetSettingsInterface()->RemoveFromStringList("GameList", "Paths", title.c_str());
                s_host_interface->GetSettingsInterface()->AddToStringList("GameList", "RecursivePaths", title.c_str());
              }
              else
              {
                s_host_interface->GetSettingsInterface()->RemoveFromStringList("GameList", "RecursivePaths",
                                                                               title.c_str());
                s_host_interface->GetSettingsInterface()->AddToStringList("GameList", "Paths", title.c_str());
              }

              s_host_interface->GetSettingsInterface()->Save();
              QueueGameListRefresh();
            });
        }

        if (MenuButton(ICON_FA_FOLDER_MINUS "  删除搜索目录",
                       "从游戏搜索列表中删除目录."))
        {
          OpenChoiceDialog(ICON_FA_FOLDER_MINUS "  删除搜索目录", false, GetGameListDirectoryOptions(false),
                           [](s32 index, const std::string& title, bool checked) {
                             if (index < 0)
                               return;

                             s_host_interface->GetSettingsInterface()->RemoveFromStringList("GameList", "Paths",
                                                                                            title.c_str());
                             s_host_interface->GetSettingsInterface()->RemoveFromStringList(
                               "GameList", "RecursivePaths", title.c_str());
                             s_host_interface->GetSettingsInterface()->Save();
                             QueueGameListRefresh();
                             CloseChoiceDialog();
                           });
        }

        MenuHeading("搜索目录");
        for (const GameList::DirectoryEntry& entry : s_host_interface->GetGameList()->GetSearchDirectories())
        {
          MenuButton(entry.path.c_str(), entry.recursive ? "扫描子目录" : "不扫描子目录",
                     false);
        }

        EndMenuButtons();
      }
      break;

      case SettingsPage::ConsoleSettings:
      {
        static constexpr auto cdrom_read_speeds =
          make_array("无（2倍速）", "2x（4倍速）", "3x（6倍速）", "4x (8倍速)", "5x (10倍速)",
                     "6x (12倍速)", "7x (14倍速)", "8x (16倍速)", "9x (18倍速)", "10x (20倍速)");

        static constexpr auto cdrom_seek_speeds = make_array("无限/瞬时", "无（正常速度）", "2x",
                                                             "3x", "4x", "5x", "6x", "7x", "8x", "9x", "10x");

        BeginMenuButtons();

        MenuHeading("控制台设置");

        settings_changed |=
          EnumChoiceButton("区域", "确定模拟硬件类型.", &s_settings_copy.region,
                           &Settings::GetConsoleRegionDisplayName, ConsoleRegion::Count);

        MenuHeading("CPU模拟（MIPS R3000A衍生产品）");

        settings_changed |= EnumChoiceButton(
          "执行模式", "确定被模拟CPU如何执行指令。建议重新编译.",
          &s_settings_copy.cpu_execution_mode, &Settings::GetCPUExecutionModeDisplayName, CPUExecutionMode::Count);

        settings_changed |=
          ToggleButton("启用超频", "当选择此选项时，将使用下面设置的时钟速度.",
                       &s_settings_copy.cpu_overclock_enable);

        s32 overclock_percent =
          s_settings_copy.cpu_overclock_enable ? static_cast<s32>(s_settings_copy.GetCPUOverclockPercent()) : 100;
        if (RangeButton("超频百分比",
                        "选择模拟硬件将运行的正常时钟速度的百分比.",
                        &overclock_percent, 10, 1000, 10, "%d%%", s_settings_copy.cpu_overclock_enable))
        {
          s_settings_copy.SetCPUOverclockPercent(static_cast<u32>(overclock_percent));
          settings_changed = true;
        }

        MenuHeading("CD-ROM模拟");

        const u32 read_speed_index =
          std::clamp<u32>(s_settings_copy.cdrom_read_speedup, 1u, static_cast<u32>(cdrom_read_speeds.size())) - 1u;
        if (MenuButtonWithValue("读取加速",
                                "按指定的系数加快CD-ROM读取速度。在某些情况下可提高装载速度 "
                                "游戏，并打破其他.",
                                cdrom_read_speeds[read_speed_index]))
        {
          ImGuiFullscreen::ChoiceDialogOptions options;
          options.reserve(cdrom_read_speeds.size());
          for (u32 i = 0; i < static_cast<u32>(cdrom_read_speeds.size()); i++)
            options.emplace_back(cdrom_read_speeds[i], i == read_speed_index);
          OpenChoiceDialog("CD-ROM 读取加速", false, std::move(options),
                           [](s32 index, const std::string& title, bool checked) {
                             if (index >= 0)
                               s_settings_copy.cdrom_read_speedup = static_cast<u32>(index) + 1;
                             CloseChoiceDialog();
                           });
        }

        const u32 seek_speed_index =
          std::min(s_settings_copy.cdrom_seek_speedup, static_cast<u32>(cdrom_seek_speeds.size()));
        if (MenuButtonWithValue("寻求加速",
                                "按指定的因数加速CD-ROM查找。可以提高加载速度在某些 "
                                "游戏，并打破其他.",
                                cdrom_seek_speeds[seek_speed_index]))
        {
          ImGuiFullscreen::ChoiceDialogOptions options;
          options.reserve(cdrom_seek_speeds.size());
          for (u32 i = 0; i < static_cast<u32>(cdrom_seek_speeds.size()); i++)
            options.emplace_back(cdrom_seek_speeds[i], i == seek_speed_index);
          OpenChoiceDialog("CD-ROM 寻求加速", false, std::move(options),
                           [](s32 index, const std::string& title, bool checked) {
                             if (index >= 0)
                               s_settings_copy.cdrom_seek_speedup = static_cast<u32>(index);
                             CloseChoiceDialog();
                           });
        }

        s32 readahead_sectors = s_settings_copy.cdrom_readahead_sectors;
        if (RangeButton(
              "读取扇区",
              "通过在工作线程上异步读取/解压缩CD数据，减少模拟中的故障。",
              &readahead_sectors, 0, 32, 1))
        {
          s_settings_copy.cdrom_readahead_sectors = static_cast<u8>(readahead_sectors);
          settings_changed = true;
        }

        settings_changed |=
          ToggleButton("启用区域检查", "模拟原始未修改控制台中的区域检查.",
                       &s_settings_copy.cdrom_region_check);
        settings_changed |= ToggleButton(
          "将图像预加载到RAM",
          "将游戏图像加载到RAM中.适用于在游戏过程中可能变得不可靠的网络路径.",
          &s_settings_copy.cdrom_load_image_to_ram);
        settings_changed |= ToggleButtonForNonSetting(
          "应用图像补丁",
          "当磁盘映像存在时自动应用补丁，目前只支持PPF.",
          "CDROM", "LoadImagePatches", false);

        MenuHeading("控制器端口");

        settings_changed |= EnumChoiceButton("转接", nullptr, &s_settings_copy.multitap_mode,
                                             &Settings::GetMultitapModeDisplayName, MultitapMode::Count);

        EndMenuButtons();
      }
      break;

      case SettingsPage::EmulationSettings:
      {
        static constexpr auto emulation_speeds =
          make_array(0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f,
                     3.0f, 3.5f, 4.0f, 4.5f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f);
        static constexpr auto get_emulation_speed_options = [](float current_speed) {
          ImGuiFullscreen::ChoiceDialogOptions options;
          options.reserve(emulation_speeds.size());
          for (const float speed : emulation_speeds)
          {
            options.emplace_back(
              (speed != 0.0f) ?
                StringUtil::StdStringFromFormat("%d%% [%d FPS (NTSC) / %d FPS (PAL)]", static_cast<int>(speed * 100.0f),
                                                static_cast<int>(60.0f * speed), static_cast<int>(50.0f * speed)) :
                "无限",
              speed == current_speed);
          }
          return options;
        };

        BeginMenuButtons();

        MenuHeading("速度控制");

#define MAKE_EMULATION_SPEED(setting_title, setting_var)                                                               \
  if (MenuButtonWithValue(                                                                                             \
        setting_title,                                                                                                 \
        "设置目标模拟速度。不能保证在所有系统上都能达到这个速度.",       \
        (setting_var != 0.0f) ? TinyString::FromFormat("%.0f%%", setting_var * 100.0f) : TinyString("无限")))     \
  {                                                                                                                    \
    OpenChoiceDialog(setting_title, false, get_emulation_speed_options(setting_var),                                   \
                     [](s32 index, const std::string& title, bool checked) {                                           \
                       if (index >= 0)                                                                                 \
                       {                                                                                               \
                         setting_var = emulation_speeds[index];                                                        \
                         s_host_interface->RunLater(SaveAndApplySettings);                                             \
                       }                                                                                               \
                       CloseChoiceDialog();                                                                            \
                     });                                                                                               \
  }

        MAKE_EMULATION_SPEED("模拟速度", s_settings_copy.emulation_speed);
        MAKE_EMULATION_SPEED("快进速度", s_settings_copy.fast_forward_speed);
        MAKE_EMULATION_SPEED("一键超频", s_settings_copy.turbo_speed);

#undef MAKE_EMULATION_SPEED

        MenuHeading("运行前/回放");

        settings_changed |=
          ToggleButton("启用倒带", "定期保存状态，以便在播放时倒带任何错误.",
                       &s_settings_copy.rewind_enable);
        settings_changed |= RangeButton(
          "倒带保存频率",
          "创建倒带状态的频率。频率越高，系统要求越高.",
          &s_settings_copy.rewind_save_frequency, 0.0f, 3600.0f, 0.1f, "%.2f Seconds", s_settings_copy.rewind_enable);
        settings_changed |=
          RangeButton("倒带保存频率",
                      "将保留多少保存以便倒带。值越高，内存需求越大.",
                      reinterpret_cast<s32*>(&s_settings_copy.rewind_save_slots), 1, 10000, 1, "%d 帧",
                      s_settings_copy.rewind_enable);

        TinyString summary;
        if (!s_settings_copy.IsRunaheadEnabled())
          summary = "禁用";
        else
          summary.Format("%u 帧", s_settings_copy.runahead_frames);

        if (MenuButtonWithValue("向前运行",
                                "提前模拟系统并回滚/回放以减少输入延迟。 "
                                "对系统要求非常高.",
                                summary))
        {
          ImGuiFullscreen::ChoiceDialogOptions options;
          for (u32 i = 0; i <= 10; i++)
          {
            if (i == 0)
              options.emplace_back("禁用", s_settings_copy.runahead_frames == i);
            else
              options.emplace_back(StringUtil::StdStringFromFormat("%u 帧", i),
                                   s_settings_copy.runahead_frames == i);
          }
          OpenChoiceDialog("向前运行", false, std::move(options),
                           [](s32 index, const std::string& title, bool checked) {
                             s_settings_copy.runahead_frames = index;
                             s_host_interface->RunLater(SaveAndApplySettings);
                             CloseChoiceDialog();
                           });
          settings_changed = true;
        }

        TinyString rewind_summary;
        if (s_settings_copy.IsRunaheadEnabled())
        {
          rewind_summary = "已禁用倒带，因为已启用提前运行。提前运行将显著增加 "
                           "系统要求.";
        }
        else if (s_settings_copy.rewind_enable)
        {
          const float duration = ((s_settings_copy.rewind_save_frequency <= std::numeric_limits<float>::epsilon()) ?
                                    (1.0f / 60.0f) :
                                    s_settings_copy.rewind_save_frequency) *
                                 static_cast<float>(s_settings_copy.rewind_save_slots);

          u64 ram_usage, vram_usage;
          System::CalculateRewindMemoryUsage(s_settings_copy.rewind_save_slots, &ram_usage, &vram_usage);
          rewind_summary.Format("回放 %u 帧，持续 %.2f 秒，最多需要%" PRIu64
                                "MB of RAM 和 %" PRIu64 "MB of VRAM.",
                                s_settings_copy.rewind_save_slots, duration, ram_usage / 1048576, vram_usage / 1048576);
        }
        else
        {
          rewind_summary =
            "未启用倒带。请注意，启用倒带可能会显著增加系统要求.";
        }

        ActiveButton(rewind_summary, false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
                     g_medium_font);

        EndMenuButtons();
      }
      break;

      case SettingsPage::BIOSSettings:
      {
        static constexpr auto config_keys = make_array("", "PathNTSCJ", "PathNTSCU", "PathPAL");
        static std::string bios_region_filenames[static_cast<u32>(ConsoleRegion::Count)];
        static std::string bios_directory;
        static bool bios_filenames_loaded = false;

        if (!bios_filenames_loaded)
        {
          for (u32 i = 0; i < static_cast<u32>(ConsoleRegion::Count); i++)
          {
            if (i == static_cast<u32>(ConsoleRegion::Auto))
              continue;
            bios_region_filenames[i] = s_host_interface->GetSettingsInterface()->GetStringValue("BIOS", config_keys[i]);
          }
          bios_directory = s_host_interface->GetBIOSDirectory();
          bios_filenames_loaded = true;
        }

        BeginMenuButtons();

        MenuHeading("BIOS 选择");

        for (u32 i = 0; i < static_cast<u32>(ConsoleRegion::Count); i++)
        {
          const ConsoleRegion region = static_cast<ConsoleRegion>(i);
          if (region == ConsoleRegion::Auto)
            continue;

          TinyString title;
          title.Format("BIOS for %s", Settings::GetConsoleRegionName(region));

          if (MenuButtonWithValue(title,
                                  SmallString::FromFormat("模拟 %s 控制台时要使用的BIOS.",
                                                          Settings::GetConsoleRegionDisplayName(region)),
                                  bios_region_filenames[i].c_str()))
          {
            ImGuiFullscreen::ChoiceDialogOptions options;
            auto images = s_host_interface->FindBIOSImagesInDirectory(s_host_interface->GetBIOSDirectory().c_str());
            options.reserve(images.size() + 1);
            options.emplace_back("Auto-Detect", bios_region_filenames[i].empty());
            for (auto& [path, info] : images)
            {
              const bool selected = bios_region_filenames[i] == path;
              options.emplace_back(std::move(path), selected);
            }

            OpenChoiceDialog(title, false, std::move(options), [i](s32 index, const std::string& path, bool checked) {
              if (index >= 0)
              {
                bios_region_filenames[i] = path;
                s_host_interface->GetSettingsInterface()->SetStringValue("BIOS", config_keys[i], path.c_str());
                s_host_interface->GetSettingsInterface()->Save();
              }
              CloseChoiceDialog();
            });
          }
        }

        if (MenuButton("BIOS 目录", bios_directory.c_str()))
        {
          OpenFileSelector("BIOS 目录", true, [](const std::string& path) {
            if (!path.empty())
            {
              bios_directory = path;
              s_host_interface->GetSettingsInterface()->SetStringValue("BIOS", "SearchDirectory", path.c_str());
              s_host_interface->GetSettingsInterface()->Save();
            }
            CloseFileSelector();
          });
        }

        MenuHeading("补丁");

        settings_changed |=
          ToggleButton("启用快速启动", "为BIOS打补丁以跳过引导动画。可安全启用.",
                       &s_settings_copy.bios_patch_fast_boot);
        settings_changed |= ToggleButton(
          "启用TTY输出", "修补BIOS以记录对printf()的调用。只有在调试时使用，才能打破游戏.",
          &s_settings_copy.bios_patch_tty_enable);

        EndMenuButtons();
      }
      break;

      case SettingsPage::ControllerSettings:
      {
        BeginMenuButtons();

        MenuHeading("输入配置文件");
        if (MenuButton(ICON_FA_FOLDER_OPEN "  加载输入配置文件",
                       "应用已保存的控制器类型和绑定配置."))
        {
          CommonHostInterface::InputProfileList profiles(s_host_interface->GetInputProfileList());
          ImGuiFullscreen::ChoiceDialogOptions options;
          options.reserve(profiles.size());
          for (CommonHostInterface::InputProfileEntry& entry : profiles)
            options.emplace_back(std::move(entry.name), false);

          auto callback = [profiles](s32 index, const std::string& title, bool checked) {
            if (index < 0)
              return;

            // needs a reload...
            s_host_interface->ApplyInputProfile(profiles[index].path.c_str());
            s_settings_copy.Load(*s_host_interface->GetSettingsInterface());
            s_host_interface->RunLater(SaveAndApplySettings);
            CloseChoiceDialog();
          };
          OpenChoiceDialog(ICON_FA_FOLDER_OPEN "  加载输入配置文件", false, std::move(options), std::move(callback));
        }

        static std::array<ControllerType, NUM_CONTROLLER_AND_CARD_PORTS> type_cache = {};
        static std::array<Controller::ButtonList, NUM_CONTROLLER_AND_CARD_PORTS> button_cache;
        static std::array<Controller::AxisList, NUM_CONTROLLER_AND_CARD_PORTS> axis_cache;
        static std::array<Controller::SettingList, NUM_CONTROLLER_AND_CARD_PORTS> setting_cache;
        static std::array<std::string,
                          NUM_CONTROLLER_AND_CARD_PORTS * CommonHostInterface::NUM_CONTROLLER_AUTOFIRE_BUTTONS>
          autofire_buttons_cache;
        TinyString section;
        TinyString key;

        std::array<TinyString, NUM_CONTROLLER_AND_CARD_PORTS> port_labels = s_settings_copy.GeneratePortLabels();

        for (u32 port = 0; port < NUM_CONTROLLER_AND_CARD_PORTS; port++)
        {
          if (port_labels[port].IsEmpty())
            continue;
          else
            MenuHeading(port_labels[port]);

          settings_changed |= EnumChoiceButton(
            TinyString::FromFormat(ICON_FA_GAMEPAD "  控制器类型##type%u", port),
            "确定插入此端口的模拟控制器.", &s_settings_copy.controller_types[port],
            &Settings::GetControllerTypeDisplayName, ControllerType::Count);

          section.Format("控制器%u", port + 1);

          const ControllerType ctype = s_settings_copy.controller_types[port];
          if (ctype != type_cache[port])
          {
            type_cache[port] = ctype;
            button_cache[port] = Controller::GetButtonNames(ctype);
            axis_cache[port] = Controller::GetAxisNames(ctype);
            setting_cache[port] = Controller::GetSettings(ctype);

            for (u32 i = 0; i < CommonHostInterface::NUM_CONTROLLER_AUTOFIRE_BUTTONS; i++)
            {
              autofire_buttons_cache[port * CommonHostInterface::NUM_CONTROLLER_AUTOFIRE_BUTTONS + i] =
                s_host_interface->GetStringSettingValue(section, TinyString::FromFormat("连发%u按键", i + 1));
            }
          }

          for (const auto& it : button_cache[port])
          {
            key.Format("按键%s", it.first.c_str());
            DrawInputBindingButton(InputBindingType::Button, section, key, it.first.c_str());
          }

          for (const auto& it : axis_cache[port])
          {
            key.Format("轴%s", std::get<0>(it).c_str());
            DrawInputBindingButton(std::get<2>(it) == Controller::AxisType::Half ? InputBindingType::HalfAxis :
                                                                                   InputBindingType::Axis,
                                   section, key, std::get<0>(it).c_str());
          }

          if (Controller::GetVibrationMotorCount(ctype) > 0)
            DrawInputBindingButton(InputBindingType::Rumble, section, "震动声", "震动声/震动");

          for (const SettingInfo& it : setting_cache[port])
            settings_changed |= SettingInfoButton(it, section);

          for (u32 autofire_index = 0; autofire_index < CommonHostInterface::NUM_CONTROLLER_AUTOFIRE_BUTTONS;
               autofire_index++)
          {
            const u32 cache_index = port * CommonHostInterface::NUM_CONTROLLER_AUTOFIRE_BUTTONS + autofire_index;

            if (MenuButtonWithValue(
                  TinyString::FromFormat("连发 %u##autofire_%u_%u", autofire_index + 1, port, autofire_index),
                  "选择用于切换此自动激发绑定的按钮.",
                  autofire_buttons_cache[cache_index].c_str()))

            {
              auto callback = [port, autofire_index, cache_index](s32 index, const std::string& title, bool checked) {
                if (index < 0)
                  return;

                auto lock = s_host_interface->GetSettingsLock();
                if (index == 0)
                {
                  s_host_interface->GetSettingsInterface()->DeleteValue(
                    TinyString::FromFormat("控制器%u", port + 1),
                    TinyString::FromFormat("连发%u按键", autofire_index + 1));
                  std::string().swap(autofire_buttons_cache[cache_index]);
                }
                else
                {
                  s_host_interface->GetSettingsInterface()->SetStringValue(
                    TinyString::FromFormat("控制器%u", port + 1),
                    TinyString::FromFormat("连发%u按键", autofire_index + 1),
                    button_cache[port][index - 1].first.c_str());
                  autofire_buttons_cache[cache_index] = button_cache[port][index - 1].first;
                }

                // needs a reload...
                s_host_interface->RunLater(SaveAndApplySettings);
                CloseChoiceDialog();
              };

              ImGuiFullscreen::ChoiceDialogOptions options;
              options.reserve(button_cache[port].size() + 1);
              options.emplace_back("(无)", autofire_buttons_cache[cache_index].empty());
              for (const auto& it : button_cache[port])
                options.emplace_back(it.first, autofire_buttons_cache[cache_index] == it.first);

              OpenChoiceDialog(ICON_FA_GAMEPAD "  选择连发按钮", false, std::move(options),
                               std::move(callback));
            }

            if (autofire_buttons_cache[cache_index].empty())
              continue;

            key.Format("连发%u", autofire_index + 1);
            DrawInputBindingButton(InputBindingType::Button, section, key,
                                   TinyString::FromFormat("连发 %u 绑定##autofire_binding_%u_%u",
                                                          autofire_index + 1, port, autofire_index),
                                   false);

            key.Format("连发%u频率", autofire_index + 1);
            int frequency = s_host_interface->GetSettingsInterface()->GetIntValue(
              section, key, CommonHostInterface::DEFAULT_AUTOFIRE_FREQUENCY);
            settings_changed |= RangeButton(TinyString::FromFormat("连发 %u 频率##autofire_frequency_%u_%u",
                                                                   autofire_index + 1, port, autofire_index),
                                            "设置连发的启动和关闭速度.", &frequency,
                                            1, 255, 1, "%d Frames");
          }
        }

        EndMenuButtons();
      }
      break;

      case SettingsPage::HotkeySettings:
      {
        BeginMenuButtons();

        TinyString last_category;
        for (const CommonHostInterface::HotkeyInfo& hotkey : s_host_interface->GetHotkeyInfoList())
        {
          if (hotkey.category != last_category)
          {
            MenuHeading(hotkey.category);
            last_category = hotkey.category;
          }

          DrawInputBindingButton(InputBindingType::Button, "热键", hotkey.name, hotkey.display_name);
        }

        EndMenuButtons();
      }
      break;

      case SettingsPage::MemoryCardSettings:
      {
        BeginMenuButtons();

        for (u32 i = 0; i < 2; i++)
        {
          MenuHeading(TinyString::FromFormat("记忆卡接口 %u", i + 1));

          settings_changed |= EnumChoiceButton(
            TinyString::FromFormat("记忆卡 %u 类型", i + 1),
            SmallString::FromFormat("设置插槽将使用哪种存储卡映像 %u.", i + 1),
            &s_settings_copy.memory_card_types[i], &Settings::GetMemoryCardTypeDisplayName, MemoryCardType::Count);

          settings_changed |= MenuButton(TinyString::FromFormat("共享存储卡 %u 路径", i + 1),
                                         s_settings_copy.memory_card_paths[i].c_str(),
                                         s_settings_copy.memory_card_types[i] == MemoryCardType::Shared);
        }

        MenuHeading("共享设置");

        settings_changed |= ToggleButton("对Sub-Images使用单卡",
                                         "当使用多盘图像(m3u/pbp)和每个游戏(标题)存储卡时, "
                                         "对所有磁盘使用单一存储卡.",
                                         &s_settings_copy.memory_card_use_playlist_title);

        static std::string memory_card_directory;
        static bool memory_card_directory_set = false;
        if (!memory_card_directory_set)
        {
          memory_card_directory = s_host_interface->GetMemoryCardDirectory();
          memory_card_directory_set = true;
        }

        if (MenuButton("记忆卡目录", memory_card_directory.c_str()))
        {
          OpenFileSelector("记忆卡目录", true, [](const std::string& path) {
            if (!path.empty())
            {
              memory_card_directory = path;
              s_settings_copy.memory_card_directory = path;
              s_host_interface->RunLater(SaveAndApplySettings);
            }
            CloseFileSelector();
          });
        }

        if (MenuButton("重置存储卡目录", "将内存卡目录重置为默认(用户目录)."))
        {
          s_settings_copy.memory_card_directory.clear();
          s_host_interface->RunLater(SaveAndApplySettings);
          memory_card_directory_set = false;
        }

        EndMenuButtons();
      }
      break;

      case SettingsPage::DisplaySettings:
      {
        BeginMenuButtons();

        MenuHeading("设备设置");

        settings_changed |=
          EnumChoiceButton("GPU渲染器", "选择用于渲染主机/游戏视觉效果的后端.",
                           &s_settings_copy.gpu_renderer, &Settings::GetRendererDisplayName, GPURenderer::Count);

        static std::string fullscreen_mode;
        static bool fullscreen_mode_set;
        if (!fullscreen_mode_set)
        {
          fullscreen_mode = s_host_interface->GetSettingsInterface()->GetStringValue("GPU", "FullscreenMode", "");
          fullscreen_mode_set = true;
        }

#ifndef _UWP
        if (MenuButtonWithValue("全屏分辨率", "选择要在全屏模式下使用的分辨率.",
                                fullscreen_mode.empty() ? "无边框全屏" : fullscreen_mode.c_str()))
        {
          HostDisplay::AdapterAndModeList aml(s_host_interface->GetDisplay()->GetAdapterAndModeList());

          ImGuiFullscreen::ChoiceDialogOptions options;
          options.reserve(aml.fullscreen_modes.size() + 1);
          options.emplace_back("无边框全屏", fullscreen_mode.empty());
          for (std::string& mode : aml.fullscreen_modes)
            options.emplace_back(std::move(mode), mode == fullscreen_mode);

          auto callback = [](s32 index, const std::string& title, bool checked) {
            if (index < 0)
              return;
            else if (index == 0)
              std::string().swap(fullscreen_mode);
            else
              fullscreen_mode = title;

            s_host_interface->GetSettingsInterface()->SetStringValue("GPU", "FullscreenMode", fullscreen_mode.c_str());
            s_host_interface->GetSettingsInterface()->Save();
            s_host_interface->AddOSDMessage("重新启动后分辨率生效。", 10.0f);
            CloseChoiceDialog();
          };
          OpenChoiceDialog(ICON_FA_TV "  全屏分辨率", false, std::move(options), std::move(callback));
        }
#endif

        switch (s_settings_copy.gpu_renderer)
        {
#ifdef _WIN32
          case GPURenderer::HardwareD3D11:
          {
            settings_changed |= ToggleButtonForNonSetting(
              "使用Blit交换链",
              "使用blit演示模型而不是翻转。某些系统可能需要此功能.", "Display",
              "UseBlitSwapChain", false);
          }
          break;
#endif

          case GPURenderer::HardwareVulkan:
          {
            settings_changed |=
              ToggleButton("线程化演示",
                           "当禁用快进或vsync时，在后台线程上显示帧.",
                           &s_settings_copy.gpu_threaded_presentation);
          }
          break;

          case GPURenderer::Software:
          {
            settings_changed |= ToggleButton("线程渲染",
                                             "使用第二个线程绘制图形。速度提升，使用安全.",
                                             &s_settings_copy.gpu_use_thread);
          }
          break;

          default:
            break;
        }

        if (!s_settings_copy.IsUsingSoftwareRenderer())
        {
          settings_changed |=
            ToggleButton("使用软件渲染器进行回读",
                         "并行运行软件渲染器以进行VRAM读回。在某些系统上，这可能会"
                         "提高性能.",
                         &s_settings_copy.gpu_use_software_renderer_for_readbacks);
        }

        settings_changed |=
          ToggleButton("启用垂直同步",
                       "将控制台帧的呈现同步到主机。启用更平滑的动画.",
                       &s_settings_copy.video_sync_enabled);

        settings_changed |= ToggleButton("同步到主机刷新率",
                                         "调整模拟速度，使控制台的刷新率与主机相匹配 "
                                         "当“垂直同步”和“音频重采样”启用时.",
                                         &s_settings_copy.sync_to_host_refresh_rate, s_settings_copy.audio_resampling);

        settings_changed |= ToggleButton("最佳帧节奏",
                                         "确保生成的每一帧都以最佳速度显示。如果"
                                         "你的速度或声音有问题请禁用",
                                         &s_settings_copy.display_all_frames);

        MenuHeading("Screen Display");

        settings_changed |= EnumChoiceButton(
          "纵横比", "更改用于在屏幕上显示控制台输出的纵横比。",
          &s_settings_copy.display_aspect_ratio, &Settings::GetDisplayAspectRatioName, DisplayAspectRatio::Count);

        settings_changed |= EnumChoiceButton(
          "裁剪模式", "确定要裁剪/隐藏的用户电视机上通常不可见的区域的大小。",
          &s_settings_copy.display_crop_mode, &Settings::GetDisplayCropModeDisplayName, DisplayCropMode::Count);

        settings_changed |=
          EnumChoiceButton("缩减取样",
                           "在显示渲染图像之前对其进行下采样。可以改进 "
                           "2D/3D混合游戏的整体图像质量.",
                           &s_settings_copy.gpu_downsample_mode, &Settings::GetDownsampleModeDisplayName,
                           GPUDownsampleMode::Count, !s_settings_copy.IsUsingSoftwareRenderer());

        settings_changed |=
          ToggleButton("线性放大", "放大显示时使用双线性过滤器，平滑图像.",
                       &s_settings_copy.display_linear_filtering, !s_settings_copy.display_integer_scaling);

        settings_changed |=
          ToggleButton("整数放大", "添加填充以确保像素大小为整数。",
                       &s_settings_copy.display_integer_scaling);

        settings_changed |= ToggleButton(
          "拉伸以适应环境", "用活动显示区域填充窗口，而不考虑长宽比.",
          &s_settings_copy.display_stretch);

        settings_changed |=
          ToggleButtonForNonSetting("内部分辨率截图",
                                    "以内部渲染分辨率保存截图，不进行后处理。",
                                    "Display", "InternalResolutionScreenshots", false);

        MenuHeading("屏幕显示");

        settings_changed |= ToggleButton("显示OSD消息", "当事件发生时显示屏幕上显示的消息.",
                                         &s_settings_copy.display_show_osd_messages);
        settings_changed |= ToggleButton(
          "“显示游戏帧率”，“在显示器的右上角显示游戏的内部帧率.",
          &s_settings_copy.display_show_fps);
        settings_changed |= ToggleButton("显示FPS",
                                         "显示系统每秒显示的帧数(或v-sync) "
                                         "在显示屏的右上角.",
                                         &s_settings_copy.display_show_vps);
        settings_changed |= ToggleButton(
          "显示速度",
          "在显示器的右上角以百分比的形式显示系统的当前模拟速度.",
          &s_settings_copy.display_show_speed);
        settings_changed |=
          ToggleButton("显示分辨率",
                       "在显示器的右上角显示系统的当前呈现分辨率.",
                       &s_settings_copy.display_show_resolution);
        settings_changed |= ToggleButtonForNonSetting(
          "显示控制器输入",
          "在显示屏的左下角显示系统的当前控制器状态.", "Display",
          "ShowInputs", false);

        EndMenuButtons();
      }
      break;

      case SettingsPage::EnhancementSettings:
      {
        static const auto resolution_scale_text_callback = [](u32 value) -> const char* {
          static constexpr std::array<const char*, 17> texts = {
            {"根据窗口大小自动", "1x", "2x", "3x (for 720p)", "4x", "5x (for 1080p)", "6x (for 1440p)",
             "7x", "8x", "9x (for 4K)", "10x", "11x", "12x", "13x", "14x", "15x", "16x"

            }};
          return (value >= texts.size()) ? "" : texts[value];
        };

        BeginMenuButtons();

        MenuHeading("渲染增强功能");

        settings_changed |= EnumChoiceButton<u32, u32>(
          "内部分辨率比例",
          "按指定的乘数缩放内部VRAM分辨率。某些游戏需要1x VRAM分辨率.",
          &s_settings_copy.gpu_resolution_scale, resolution_scale_text_callback, 17);
        settings_changed |= EnumChoiceButton(
          "纹理过滤",
          "平滑3D对象上放大纹理的块状。会有更大的效果 "
          "在更高分辨率尺度上.",
          &s_settings_copy.gpu_texture_filter, &Settings::GetTextureFilterDisplayName, GPUTextureFilter::Count);
        settings_changed |=
          ToggleButton("真彩色渲染",
                       "禁用抖动并使用每通道完整的8位颜色信息。可能破裂 "
                       "在某些游戏中渲染.",
                       &s_settings_copy.gpu_true_color);
        settings_changed |= ToggleButton(
          "缩放抖动",
          "根据内部渲染分辨率缩放抖动模式，使其不那么明显. "
          "通常启用是安全的.",
          &s_settings_copy.gpu_scaled_dithering, s_settings_copy.gpu_resolution_scale > 1);
        settings_changed |= ToggleButton(
          "宽屏修改", "在3D游戏中将视野从4:3增加到所选的显示长宽比.",
          &s_settings_copy.gpu_widescreen_hack);

        MenuHeading("显示增强");

        settings_changed |=
          ToggleButton("禁用隔行扫描",
                       "禁用GPU中的隔行渲染和显示。某些游戏可以通过这种方式以480p渲染, "
                       "但其他地方会崩溃.",
                       &s_settings_copy.gpu_disable_interlacing);
        settings_changed |= ToggleButton(
          "强制NTSC计时",
          "强制PAL游戏以NTSC定时运行，即60hz。一些PAL游戏将在 \"正常的\" "
          "速度，而其他将打破.",
          &s_settings_copy.gpu_force_ntsc_timings);
        settings_changed |=
          ToggleButton("24位显示器强制4:3",
                       "在显示24位内容(通常是fmv)时，切换回4:3的显示宽高比.",
                       &s_settings_copy.display_force_4_3_for_24bit);
        settings_changed |= ToggleButton(
          "用于24位显示的色度平滑",
          "平滑24位内容（通常是FMV）中颜色转换之间的块状。仅适用 "
          "到硬件渲染器.",
          &s_settings_copy.gpu_24bit_chroma_smoothing);

        MenuHeading("PGXP (精确几何变换管道");

        settings_changed |=
          ToggleButton("PGXP 几何校正",
                       "减少 \"不稳定的\" 通过尝试通过内存保留分数分量来生成多边形 "
                       "转移.",
                       &s_settings_copy.gpu_pgxp_enable);
        settings_changed |=
          ToggleButton("PGXP 纹理校正",
                       "使用透视校正插值纹理坐标和颜色，理顺 "
                       "扭曲的纹理.",
                       &s_settings_copy.gpu_pgxp_texture_correction, s_settings_copy.gpu_pgxp_enable);
        settings_changed |=
          ToggleButton("PGXP 消隐校正",
                       "提高多边形剔除的精度，减少几何体中的孔数.",
                       &s_settings_copy.gpu_pgxp_culling, s_settings_copy.gpu_pgxp_enable);
        settings_changed |=
          ToggleButton("PGXP 保持投影精度",
                       "增加PGXP数据后期投影的精度。可以改善某些游戏的视觉效果.",
                       &s_settings_copy.gpu_pgxp_preserve_proj_fp, s_settings_copy.gpu_pgxp_enable);
        settings_changed |= ToggleButton(
          "PGXP 深度缓冲区", "通过深度测试减少多边形Z形战斗。与游戏的兼容性低.",
          &s_settings_copy.gpu_pgxp_depth_buffer,
          s_settings_copy.gpu_pgxp_enable && s_settings_copy.gpu_pgxp_texture_correction);
        settings_changed |= ToggleButton("PGXP CPU 模式", "对所有指令使用PGXP，而不仅仅是内存操作.",
                                         &s_settings_copy.gpu_pgxp_cpu, s_settings_copy.gpu_pgxp_enable);

        EndMenuButtons();
      }
      break;

      case SettingsPage::AudioSettings:
      {
        BeginMenuButtons();

        MenuHeading("音频控制");

        settings_changed |= RangeButton("输出音量", "控制主机上播放的音频音量.",
                                        &s_settings_copy.audio_output_volume, 0, 100, 1, "%d%%");
        settings_changed |= RangeButton("快进音量",
                                        "控制快进时主机上播放的音频音量.",
                                        &s_settings_copy.audio_fast_forward_volume, 0, 100, 1, "%d%%");
        settings_changed |= ToggleButton("静音所有声音", "防止模拟器产生任何可听见的声音.",
                                         &s_settings_copy.audio_output_muted);
        settings_changed |= ToggleButton("将CD音频静音",
                                         "从CD-ROM中强制静音CD-DA和XA音频。可用于 "
                                         "在某些游戏中禁用背景音乐.",
                                         &s_settings_copy.cdrom_mute_cd_audio);

        MenuHeading("后端设置");

        settings_changed |= EnumChoiceButton(
          "音频后端",
          "音频后端决定模拟器产生的帧如何提交给主机.",
          &s_settings_copy.audio_backend, &Settings::GetAudioBackendDisplayName, AudioBackend::Count);
        settings_changed |= RangeButton(
          "缓冲区大小", "缓冲区大小决定了主机将提取的音频块的大小.",
          reinterpret_cast<s32*>(&s_settings_copy.audio_buffer_size), 1024, 8192, 128, "%d Frames");

        settings_changed |= ToggleButton("同步到输出",
                                         "基于音频后端拉动音频来限制模拟速度 "
                                         "帧.能够减少爆裂的机会.",
                                         &s_settings_copy.audio_sync_enabled);
        settings_changed |= ToggleButton(
          "重新采样",
          "当以100%的速度运行时，从目标速度重新采样音频，而不是丢弃帧.",
          &s_settings_copy.audio_resampling);

        EndMenuButtons();
      }
      break;

      case SettingsPage::AchievementsSetings:
      {
#ifdef WITH_CHEEVOS
        BeginMenuButtons();

        MenuHeading("设置");
        if (ToggleButtonForNonSetting(ICON_FA_TROPHY "  启用追溯成就",
                                      "启用并登录后，DuckStation将在启动时扫描成就.",
                                      "Cheevos", "启用", false))
        {
          settings_changed = true;
          s_host_interface->RunLater([]() {
            if (!ConfirmChallengeModeEnable())
              s_host_interface->GetSettingsInterface()->SetBoolValue("Cheevos", "启用", false);
          });
        }

        settings_changed |= ToggleButtonForNonSetting(
          ICON_FA_USER_FRIENDS "  丰富的状态",
          "启用后，将收集丰富的状态信息并将其发送到受支持的服务器.",
          "Cheevos", "RichPresence", true);
        settings_changed |=
          ToggleButtonForNonSetting(ICON_FA_STETHOSCOPE "  测试模式",
                                    "启用后，DuckStation将假设所有成就都已锁定，而不是 "
                                    "向服务器发送任何解锁通知.",
                                    "Cheevos", "TestMode", false);
        settings_changed |=
          ToggleButtonForNonSetting(ICON_FA_MEDAL "  测试非官方成就",
                                    "启用后，DuckStation将列出非官方集合中的成就。这些 "
                                    "RetroAchievement不跟踪成就.",
                                    "Cheevos", "UnofficialTestMode", false);
        settings_changed |= ToggleButtonForNonSetting(ICON_FA_COMPACT_DISC "  使用播放列表中的第一张光盘",
                                                      "启用后，播放列表中的第一张光盘将用于"
                                                      "成就，无论哪个光盘处于活动状态.",
                                                      "Cheevos", "UseFirstDiscFromPlaylist", true);

        if (ToggleButtonForNonSetting(ICON_FA_HARD_HAT "  硬核模式",
                                      "\"挑战\" 成就模式。禁用保存状态、欺骗和减速 "
                                      "功能，但你会获得双倍的成就点.",
                                      "Cheevos", "ChallengeMode", false))
        {
          s_host_interface->RunLater([]() {
            if (!ConfirmChallengeModeEnable())
              s_host_interface->GetSettingsInterface()->SetBoolValue("Cheevos", "ChallengeMode", false);
          });
        }

        MenuHeading("账户");
        if (Cheevos::IsLoggedIn())
        {
          ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyle().Colors[ImGuiCol_Text]);
          ActiveButton(SmallString::FromFormat(ICON_FA_USER "  用户名: %s", Cheevos::GetUsername().c_str()), false,
                       false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

          Timestamp ts;
          TinyString ts_string;
          ts.SetUnixTimestamp(StringUtil::FromChars<u64>(s_host_interface->GetSettingsInterface()->GetStringValue(
                                                           "Cheevos", "LoginTimestamp", "0"))
                                .value_or(0));
          ts.ToString(ts_string, "%Y-%m-%d %H:%M:%S");
          ActiveButton(SmallString::FromFormat(ICON_FA_CLOCK "  Login token generated on %s", ts_string.GetCharArray()),
                       false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          ImGui::PopStyleColor();

          if (MenuButton(ICON_FA_KEY "  注销", "注销RetroAchievement."))
            Cheevos::Logout();
        }
        else if (Cheevos::IsActive())
        {
          ActiveButton(ICON_FA_USER "  未登录", false, false,
                       ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

          if (MenuButton(ICON_FA_KEY "  登录", "登录RetroAchievement."))
            ImGui::OpenPopup("成就登录");

          DrawAchievementsLoginWindow();
        }
        else
        {
          ActiveButton(ICON_FA_USER "  成就被禁用.", false, false,
                       ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
        }

        MenuHeading("当前游戏");
        if (Cheevos::HasActiveGame())
        {
          ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyle().Colors[ImGuiCol_Text]);
          ActiveButton(TinyString::FromFormat(ICON_FA_BOOKMARK "  游戏ID: %u", Cheevos::GetGameID()), false, false,
                       ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          ActiveButton(TinyString::FromFormat(ICON_FA_BOOK "  游戏标题: %s", Cheevos::GetGameTitle().c_str()), false,
                       false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          ActiveButton(
            TinyString::FromFormat(ICON_FA_DESKTOP "  游戏开发人员: %s", Cheevos::GetGameDeveloper().c_str()), false,
            false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          ActiveButton(
            TinyString::FromFormat(ICON_FA_DESKTOP "  游戏发行商: %s", Cheevos::GetGamePublisher().c_str()), false,
            false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          ActiveButton(TinyString::FromFormat(ICON_FA_TROPHY "  成就: %u (%u 点数)",
                                              Cheevos::GetAchievementCount(), Cheevos::GetMaximumPointsForGame()),
                       false, false, ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

          const std::string& rich_presence_string = Cheevos::GetRichPresenceString();
          if (!rich_presence_string.empty())
          {
            ActiveButton(SmallString::FromFormat(ICON_FA_MAP "  %s", rich_presence_string.c_str()), false, false,
                         ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          }
          else
          {
            ActiveButton(ICON_FA_MAP "  丰富的状态不活动或不受支持.", false, false,
                         ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
          }

          ImGui::PopStyleColor();
        }
        else
        {
          ActiveButton(ICON_FA_BAN "  游戏未加载或无RetroAchievement可用.", false, false,
                       ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
        }

        EndMenuButtons();
#else
        BeginMenuButtons();
        ActiveButton(ICON_FA_BAN "  此版本未使用RetroAcivements支持进行编译.", false, false,
                     ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
        EndMenuButtons();
#endif
        // ImGuiFullscreen::moda
        // if (ImGui::BeginPopup("))
      }
      break;

      case SettingsPage::AdvancedSettings:
      {
        BeginMenuButtons();

        MenuHeading("日志记录设置");
        settings_changed |=
          EnumChoiceButton("日志级别", "设置记录的消息的详细程度。更高级别将记录更多消息.",
                           &s_settings_copy.log_level, &Settings::GetLogLevelDisplayName, LOGLEVEL_COUNT);
        settings_changed |= ToggleButton("登录到系统控制台", "将消息记录到控制台窗口.",
                                         &s_settings_copy.log_to_console);
        settings_changed |= ToggleButton("日志到调试控制台", "将消息记录到支持的调试控制台.",
                                         &s_settings_copy.log_to_debug);
        settings_changed |= ToggleButton("记录到文件", "将消息记录到duckstation。登录用户目录.",
                                         &s_settings_copy.log_to_file);

        MenuHeading("调试设置");

        bool debug_menu = s_debug_menu_enabled;
        if (ToggleButton("启用调试菜单", "显示带有其他统计信息和快速设置的调试菜单栏.",
                         &debug_menu))
        {
          s_host_interface->RunLater([debug_menu]() { SetDebugMenuEnabled(debug_menu); });
        }

        settings_changed |=
          ToggleButton("禁用所有增强功能", "暂时禁用所有增强功能，在测试时非常有用.",
                       &s_settings_copy.disable_all_enhancements);

        settings_changed |= ToggleButton(
          "使用调试GPU设备", "在主机的渲染器API支持时启用调试。仅供开发者使用.",
          &s_settings_copy.gpu_use_debug_device);

#ifdef _WIN32
        settings_changed |=
          ToggleButton("提高计时器分辨率", "以电池寿命为代价实现更精确的帧起搏.",
                       &s_settings_copy.increase_timer_resolution);
#endif

        settings_changed |= ToggleButtonForNonSetting("允许在没有SBI文件的情况下启动",
                                                      "允许在没有子频道信息的情况下加载受保护的游戏.",
                                                      "CDROM", "AllowBootingWithoutSBIFile", false);

        settings_changed |= ToggleButtonForNonSetting("创建保存状态备份",
                                                      "保存到备份文件时重命名现有的保存状态。",
                                                      "General", "CreateSaveStateBackups", false);

        MenuHeading("显示设置");
        settings_changed |=
          ToggleButton("显示状态指示器", "当turbo激活或暂停时显示持久图标.",
                       &g_settings.display_show_status_indicators);
        settings_changed |= ToggleButton("显示增强设置",
                                         "在屏幕右下角显示增强设置.",
                                         &g_settings.display_show_enhancements);
        settings_changed |= RangeButton(
          "显示FPS限制", "限制屏幕上显示的帧数。这些帧仍被渲染.",
          &s_settings_copy.display_max_fps, 0.0f, 500.0f, 1.0f, "%.2f FPS");

        MenuHeading("PGXP 设置");

        settings_changed |= ToggleButton(
          "启用PGXP顶点缓存", "使用屏幕位置解析PGXP数据。可以改善某些游戏的视觉效果.",
          &s_settings_copy.gpu_pgxp_vertex_cache, s_settings_copy.gpu_pgxp_enable);
        settings_changed |= RangeButton(
          "PGXP 几何图形公差",
          "设置超过时丢弃精确值的阈值。可能有助于解决某些游戏中的故障.",
          &s_settings_copy.gpu_pgxp_tolerance, -1.0f, 10.0f, 0.1f, "%.1f Pixels", s_settings_copy.gpu_pgxp_enable);
        settings_changed |= RangeButton(
          "PGXP 深度清除阈值",
          "设置丢弃模拟深度缓冲区的阈值。在某些游戏中可能会有所帮助.",
          &s_settings_copy.gpu_pgxp_tolerance, 0.0f, 4096.0f, 1.0f, "%.1f", s_settings_copy.gpu_pgxp_enable);

        MenuHeading("纹理转储/替换");

        settings_changed |= ToggleButton("启用VRAM写入纹理替换",
                                         "支持在支持的游戏中替换背景纹理.",
                                         &s_settings_copy.texture_replacements.enable_vram_write_replacements);
        settings_changed |= ToggleButton("预加载替换纹理",
                                         "将所有替换纹理加载到RAM中，减少运行时的抖动.",
                                         &s_settings_copy.texture_replacements.preload_textures,
                                         s_settings_copy.texture_replacements.AnyReplacementsEnabled());
        settings_changed |=
          ToggleButton("转储可重复VRAM写入", "将可替换的纹理写入转储目录.",
                       &s_settings_copy.texture_replacements.dump_vram_writes);
        settings_changed |=
          ToggleButton("设置VRAM写转储Alpha通道", "清除VRAM写转储中的掩码/透明度位.",
                       &s_settings_copy.texture_replacements.dump_vram_write_force_alpha_channel,
                       s_settings_copy.texture_replacements.dump_vram_writes);

        MenuHeading("CPU 模拟");

        settings_changed |=
          ToggleButton("启用复合编译器ICache",
                       "在重新编译程序中模拟CPU的指令缓存。可以帮助游戏运行速度过快.",
                       &s_settings_copy.cpu_recompiler_icache);
        settings_changed |= ToggleButton("启用重新编译器内存异常",
                                         "启用对齐和总线异常。任何已知游戏都不需要.",
                                         &s_settings_copy.cpu_recompiler_memory_exceptions);
        settings_changed |= ToggleButton(
          "启用重新编译程序块链接",
          "性能增强-直接在块之间跳转，而不是返回到调度器.",
          &s_settings_copy.cpu_recompiler_block_linking);
        settings_changed |= EnumChoiceButton("重组器快速内存访问",
                                             "避免了对C++代码的调用，大大加快了重新编译速度.",
                                             &s_settings_copy.cpu_fastmem_mode, &Settings::GetCPUFastmemModeDisplayName,
                                             CPUFastmemMode::Count, !s_settings_copy.cpu_recompiler_memory_exceptions);

        EndMenuButtons();
      }
      break;
    }

    if (settings_changed)
      s_host_interface->RunLater(SaveAndApplySettings);
  }

  EndFullscreenWindow();
}

void DrawQuickMenu(MainWindowType type)
{
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 display_size(ImGui::GetIO().DisplaySize);
  dl->AddRectFilled(ImVec2(0.0f, 0.0f), display_size, IM_COL32(0x21, 0x21, 0x21, 200));

  // title info
  {
    const std::string& title = System::GetRunningTitle();
    const std::string& code = System::GetRunningCode();

    SmallString subtitle;
    if (!code.empty())
      subtitle.Format("%s - ", code.c_str());
    subtitle.AppendString(FileSystem::GetFileNameFromPath(System::GetRunningPath()));

    const ImVec2 title_size(
      g_large_font->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(), -1.0f, title.c_str()));
    const ImVec2 subtitle_size(
      g_medium_font->CalcTextSizeA(g_medium_font->FontSize, std::numeric_limits<float>::max(), -1.0f, subtitle));

    ImVec2 title_pos(display_size.x - LayoutScale(20.0f + 50.0f + 20.0f) - title_size.x,
                     display_size.y - LayoutScale(20.0f + 50.0f));
    ImVec2 subtitle_pos(display_size.x - LayoutScale(20.0f + 50.0f + 20.0f) - subtitle_size.x,
                        title_pos.y + g_large_font->FontSize + LayoutScale(4.0f));
    float rp_height = 0.0f;

#ifdef WITH_CHEEVOS
    if (Cheevos::IsActive())
    {
      const std::string& rp = Cheevos::GetRichPresenceString();
      if (!rp.empty())
      {
        const float wrap_width = LayoutScale(350.0f);
        const ImVec2 rp_size = g_medium_font->CalcTextSizeA(g_medium_font->FontSize, std::numeric_limits<float>::max(),
                                                            wrap_width, rp.data(), rp.data() + rp.size());
        rp_height = rp_size.y + LayoutScale(4.0f);

        const ImVec2 rp_pos(display_size.x - LayoutScale(20.0f + 50.0f + 20.0f) - rp_size.x - rp_height,
                            subtitle_pos.y + LayoutScale(4.0f));

        title_pos.x -= rp_height;
        title_pos.y -= rp_height;
        subtitle_pos.x -= rp_height;
        subtitle_pos.y -= rp_height;

        dl->AddText(g_medium_font, g_medium_font->FontSize, rp_pos, IM_COL32(255, 255, 255, 255), rp.data(),
                    rp.data() + rp.size(), wrap_width);
      }
    }
#endif

    dl->AddText(g_large_font, g_large_font->FontSize, title_pos, IM_COL32(255, 255, 255, 255), title.c_str());
    dl->AddText(g_medium_font, g_medium_font->FontSize, subtitle_pos, IM_COL32(255, 255, 255, 255), subtitle);

    const ImVec2 image_min(display_size.x - LayoutScale(20.0f + 50.0f) - rp_height,
                           display_size.y - LayoutScale(20.0f + 50.0f) - rp_height);
    const ImVec2 image_max(image_min.x + LayoutScale(50.0f) + rp_height, image_min.y + LayoutScale(50.0f) + rp_height);
    dl->AddImage(GetCoverForCurrentGame()->GetHandle(), image_min, image_max);
  }

  const ImVec2 window_size(LayoutScale(500.0f, LAYOUT_SCREEN_HEIGHT));
  const ImVec2 window_pos(0.0f, display_size.y - window_size.y);
  if (BeginFullscreenWindow(window_pos, window_size, "pause_menu", ImVec4(0.0f, 0.0f, 0.0f, 0.0f), 0.0f, 10.0f,
                            ImGuiWindowFlags_NoBackground))
  {
    BeginMenuButtons(13, 1.0f, ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                     ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING,
                     ImGuiFullscreen::LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

    if (ActiveButton(ICON_FA_PLAY "  重新进入游戏", false) || WantsToCloseMenu())
      CloseQuickMenu();

    if (ActiveButton(ICON_FA_FAST_FORWARD "  快进", false))
    {
      s_host_interface->RunLater(
        []() { s_host_interface->SetFastForwardEnabled(!s_host_interface->IsFastForwardEnabled()); });
      CloseQuickMenu();
    }

#ifdef WITH_CHEEVOS
    const bool achievements_enabled = Cheevos::HasActiveGame() && (Cheevos::GetAchievementCount() > 0);
    if (ActiveButton(ICON_FA_TROPHY "  成就", false, achievements_enabled))
      OpenAchievementsWindow();

    const bool leaderboards_enabled = Cheevos::HasActiveGame() && (Cheevos::GetLeaderboardCount() > 0);
    if (ActiveButton(ICON_FA_STOPWATCH "  排行榜", false, leaderboards_enabled))
      OpenLeaderboardsWindow();

#else
    ActiveButton(ICON_FA_TROPHY "  成就", false, false);
    ActiveButton(ICON_FA_STOPWATCH "  游戏排行榜", false, false);
#endif

    if (ActiveButton(ICON_FA_CAMERA "  保存截图", false))
    {
      CloseQuickMenu();
      s_host_interface->RunLater([]() { s_host_interface->SaveScreenshot(); });
    }

    if (ActiveButton(ICON_FA_UNDO "  加载状态", false, !IsCheevosHardcoreModeActive()))
    {
      s_current_main_window = MainWindowType::None;
      OpenSaveStateSelector(true);
    }

    if (ActiveButton(ICON_FA_SAVE "  保存状态", false))
    {
      s_current_main_window = MainWindowType::None;
      OpenSaveStateSelector(false);
    }

    if (ActiveButton(ICON_FA_FROWN_OPEN "  作弊列表", false, !IsCheevosHardcoreModeActive()))
    {
      s_current_main_window = MainWindowType::None;
      DoCheatsMenu();
    }

    if (ActiveButton(ICON_FA_GAMEPAD "  切换模拟", false))
    {
      CloseQuickMenu();
      DoToggleAnalogMode();
    }

    if (ActiveButton(ICON_FA_COMPACT_DISC "  更改光盘", false))
    {
      s_current_main_window = MainWindowType::None;
      DoChangeDisc();
    }

    if (ActiveButton(ICON_FA_SLIDERS_H "  设置(非专业人员慎用)", false))
      s_current_main_window = MainWindowType::Settings;

    if (ActiveButton(ICON_FA_SYNC "  重置系统", false))
    {
      CloseQuickMenu();
      s_host_interface->RunLater(DoReset);
    }

    if (ActiveButton(ICON_FA_POWER_OFF "  退出游戏", false))
    {
      CloseQuickMenu();
      s_host_interface->RunLater(DoPowerOff);
    }

    EndMenuButtons();

    EndFullscreenWindow();
  }
}

void InitializePlaceholderSaveStateListEntry(SaveStateListEntry* li, s32 slot, bool global)
{
  if (global)
    li->title = StringUtil::StdStringFromFormat("全局插槽 %d##global_slot_%d", slot, slot);
  else
    li->title =
      StringUtil::StdStringFromFormat("%s 插槽 %d##game_slot_%d", System::GetRunningTitle().c_str(), slot, slot);

  li->summary = "无保存状态";

  std::string().swap(li->path);
  std::string().swap(li->media_path);
  li->slot = slot;
  li->global = global;
}

void InitializeSaveStateListEntry(SaveStateListEntry* li, CommonHostInterface::ExtendedSaveStateInfo* ssi)
{
  if (ssi->global)
  {
    li->title =
      StringUtil::StdStringFromFormat("全局保存 %d - %s##global_slot_%d", ssi->slot, ssi->title.c_str(), ssi->slot);
  }
  else
  {
    li->title = StringUtil::StdStringFromFormat("%s 插槽 %d##game_slot_%d", ssi->title.c_str(), ssi->slot, ssi->slot);
  }

  li->summary =
    StringUtil::StdStringFromFormat("%s - 保存 %s", ssi->game_code.c_str(),
                                    Timestamp::FromUnixTimestamp(ssi->timestamp).ToString("%c").GetCharArray());

  li->slot = ssi->slot;
  li->global = ssi->global;
  li->path = std::move(ssi->path);
  li->media_path = std::move(ssi->media_path);

  li->preview_texture.reset();
  if (ssi && !ssi->screenshot_data.empty())
  {
    li->preview_texture = s_host_interface->GetDisplay()->CreateTexture(
      ssi->screenshot_width, ssi->screenshot_height, 1, 1, 1, HostDisplayPixelFormat::RGBA8,
      ssi->screenshot_data.data(), sizeof(u32) * ssi->screenshot_width, false);
  }
  else
  {
    li->preview_texture = s_host_interface->GetDisplay()->CreateTexture(
      PLACEHOLDER_ICON_WIDTH, PLACEHOLDER_ICON_HEIGHT, 1, 1, 1, HostDisplayPixelFormat::RGBA8, PLACEHOLDER_ICON_DATA,
      sizeof(u32) * PLACEHOLDER_ICON_WIDTH, false);
  }

  if (!li->preview_texture)
    Log_ErrorPrintf("无法将保存状态图像上载到GPU");
}

void PopulateSaveStateListEntries()
{
  s_save_state_selector_slots.clear();

  if (s_save_state_selector_loading)
  {
    std::optional<CommonHostInterface::ExtendedSaveStateInfo> ssi = s_host_interface->GetUndoSaveStateInfo();
    if (ssi)
    {
      SaveStateListEntry li;
      InitializeSaveStateListEntry(&li, &ssi.value());
      li.title = "撤消加载状态";
      li.summary = "恢复上次加载状态之前的系统状态.";
      s_save_state_selector_slots.push_back(std::move(li));
    }
  }

  if (!System::GetRunningCode().empty())
  {
    for (s32 i = 1; i <= CommonHostInterface::PER_GAME_SAVE_STATE_SLOTS; i++)
    {
      std::optional<CommonHostInterface::ExtendedSaveStateInfo> ssi =
        s_host_interface->GetExtendedSaveStateInfo(System::GetRunningCode().c_str(), i);

      SaveStateListEntry li;
      if (ssi)
        InitializeSaveStateListEntry(&li, &ssi.value());
      else
        InitializePlaceholderSaveStateListEntry(&li, i, false);

      s_save_state_selector_slots.push_back(std::move(li));
    }
  }

  for (s32 i = 1; i <= CommonHostInterface::GLOBAL_SAVE_STATE_SLOTS; i++)
  {
    std::optional<CommonHostInterface::ExtendedSaveStateInfo> ssi =
      s_host_interface->GetExtendedSaveStateInfo(nullptr, i);

    SaveStateListEntry li;
    if (ssi)
      InitializeSaveStateListEntry(&li, &ssi.value());
    else
      InitializePlaceholderSaveStateListEntry(&li, i, true);

    s_save_state_selector_slots.push_back(std::move(li));
  }
}

#if 0
void DrawSaveStateSelector(bool is_loading, bool fullscreen)
{
  const HostDisplayTexture* selected_texture = s_placeholder_texture.get();
  if (!BeginFullscreenColumns())
  {
    EndFullscreenColumns();
    return;
  }

  // drawn back the front so the hover changes the image
  if (BeginFullscreenColumnWindow(570.0f, LAYOUT_SCREEN_WIDTH, "save_state_selector_slots"))
  {
    BeginMenuButtons(static_cast<u32>(s_save_state_selector_slots.size()), true);

    for (const SaveStateListEntry& entry : s_save_state_selector_slots)
    {
      if (MenuButton(entry.title.c_str(), entry.summary.c_str()))
      {
        const std::string& path = entry.path;
        s_host_interface->RunLater([path]() { s_host_interface->LoadState(path.c_str()); });
      }

      if (ImGui::IsItemHovered())
        selected_texture = entry.preview_texture.get();
    }

    EndMenuButtons();
  }
  EndFullscreenColumnWindow();

  if (BeginFullscreenColumnWindow(0.0f, 570.0f, "save_state_selector_preview", ImVec4(0.11f, 0.15f, 0.17f, 1.00f)))
  {
    ImGui::SetCursorPos(LayoutScale(20.0f, 20.0f));
    ImGui::PushFont(g_large_font);
    ImGui::TextUnformatted(is_loading ? ICON_FA_FOLDER_OPEN "  Load State" : ICON_FA_SAVE "  Save State");
    ImGui::PopFont();

    ImGui::SetCursorPos(LayoutScale(ImVec2(85.0f, 160.0f)));
    ImGui::Image(selected_texture ? selected_texture->GetHandle() : s_placeholder_texture->GetHandle(),
      LayoutScale(ImVec2(400.0f, 400.0f)));

    ImGui::SetCursorPosY(LayoutScale(670.0f));
    BeginMenuButtons(1, false);
    if (ActiveButton(ICON_FA_BACKWARD "  Back", false))
      ReturnToMainWindow();
    EndMenuButtons();
  }
  EndFullscreenColumnWindow();

  EndFullscreenColumns();
}
#endif

void OpenSaveStateSelector(bool is_loading)
{
  s_save_state_selector_loading = is_loading;
  s_save_state_selector_open = true;
  s_save_state_selector_slots.clear();
  PopulateSaveStateListEntries();
}

void CloseSaveStateSelector()
{
  s_save_state_selector_slots.clear();
  s_save_state_selector_open = false;
  ReturnToMainWindow();
}

void DrawSaveStateSelector(bool is_loading, bool fullscreen)
{
  if (fullscreen)
  {
    if (!BeginFullscreenColumns())
    {
      EndFullscreenColumns();
      return;
    }

    if (!BeginFullscreenColumnWindow(0.0f, LAYOUT_SCREEN_WIDTH, "save_state_selector_slots"))
    {
      EndFullscreenColumnWindow();
      EndFullscreenColumns();
      return;
    }
  }
  else
  {
    const char* window_title = is_loading ? "加载状态" : "保存状态";

    ImGui::PushFont(g_large_font);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(ImGuiFullscreen::LAYOUT_MENU_BUTTON_X_PADDING,
                                                                ImGuiFullscreen::LAYOUT_MENU_BUTTON_Y_PADDING));

    ImGui::SetNextWindowSize(LayoutScale(1000.0f, 680.0f));
    ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::OpenPopup(window_title);
    bool is_open = !WantsToCloseMenu();
    if (!ImGui::BeginPopupModal(window_title, &is_open,
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove) ||
        !is_open)
    {
      ImGui::PopStyleVar(2);
      ImGui::PopFont();
      CloseSaveStateSelector();
      return;
    }
  }

  BeginMenuButtons();

  constexpr float padding = 10.0f;
  constexpr float button_height = 96.0f;
  constexpr float max_image_width = 96.0f;
  constexpr float max_image_height = 96.0f;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (const SaveStateListEntry& entry : s_save_state_selector_slots)
  {
    ImRect bb;
    bool visible, hovered;
    bool pressed = MenuButtonFrame(entry.title.c_str(), true, button_height, &visible, &hovered, &bb.Min, &bb.Max);
    if (!visible)
      continue;

    ImVec2 pos(bb.Min);

    // use aspect ratio of screenshot to determine height
    const HostDisplayTexture* image = entry.preview_texture ? entry.preview_texture.get() : s_placeholder_texture.get();
    const float image_height =
      max_image_width / (static_cast<float>(image->GetWidth()) / static_cast<float>(image->GetHeight()));
    const float image_margin = (max_image_height - image_height) / 2.0f;
    const ImRect image_bb(ImVec2(pos.x, pos.y + LayoutScale(image_margin)),
                          pos + LayoutScale(max_image_width, image_margin + image_height));
    pos.x += LayoutScale(max_image_width + padding);

    dl->AddImage(static_cast<ImTextureID>(entry.preview_texture ? entry.preview_texture->GetHandle() :
                                                                  s_placeholder_texture->GetHandle()),
                 image_bb.Min, image_bb.Max);

    ImRect text_bb(pos, ImVec2(bb.Max.x, pos.y + g_large_font->FontSize));
    ImGui::PushFont(g_large_font);
    ImGui::RenderTextClipped(text_bb.Min, text_bb.Max, entry.title.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f),
                             &text_bb);
    ImGui::PopFont();

    ImGui::PushFont(g_medium_font);

    if (!entry.summary.empty())
    {
      text_bb.Min.y = text_bb.Max.y + LayoutScale(4.0f);
      text_bb.Max.y = text_bb.Min.y + g_medium_font->FontSize;
      ImGui::RenderTextClipped(text_bb.Min, text_bb.Max, entry.summary.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f),
                               &text_bb);
    }

    if (!entry.path.empty())
    {
      text_bb.Min.y = text_bb.Max.y + LayoutScale(4.0f);
      text_bb.Max.y = text_bb.Min.y + g_medium_font->FontSize;
      ImGui::RenderTextClipped(text_bb.Min, text_bb.Max, entry.path.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f),
                               &text_bb);
    }

    if (!entry.media_path.empty())
    {
      text_bb.Min.y = text_bb.Max.y + LayoutScale(4.0f);
      text_bb.Max.y = text_bb.Min.y + g_medium_font->FontSize;
      ImGui::RenderTextClipped(text_bb.Min, text_bb.Max, entry.media_path.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f),
                               &text_bb);
    }

    ImGui::PopFont();

    if (pressed)
    {
      if (is_loading)
      {
        const std::string& path = entry.path;
        s_host_interface->RunLater([path]() {
          if (path.empty())
            s_host_interface->UndoLoadState();
          else
            s_host_interface->LoadState(path.c_str());

          CloseSaveStateSelector();
        });
      }
      else
      {
        const s32 slot = entry.slot;
        const bool global = entry.global;
        s_host_interface->RunLater([slot, global]() {
          s_host_interface->SaveState(global, slot);
          CloseSaveStateSelector();
        });
      }
    }
  }

  EndMenuButtons();

  if (fullscreen)
  {
    EndFullscreenColumnWindow();
    EndFullscreenColumns();
  }
  else
  {
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
  }
}

void DrawGameListWindow()
{
  const GameListEntry* selected_entry = nullptr;

  if (!BeginFullscreenColumns())
  {
    EndFullscreenColumns();
    return;
  }

  if (BeginFullscreenColumnWindow(450.0f, LAYOUT_SCREEN_WIDTH, "game_list_entries"))
  {
    const ImVec2 image_size(LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT, LAYOUT_MENU_BUTTON_HEIGHT));

    BeginMenuButtons();

    SmallString summary;

    for (const GameListEntry* entry : s_game_list_sorted_entries)
    {
      ImRect bb;
      bool visible, hovered;
      bool pressed =
        MenuButtonFrame(entry->path.c_str(), true, LAYOUT_MENU_BUTTON_HEIGHT, &visible, &hovered, &bb.Min, &bb.Max);
      if (!visible)
        continue;

      HostDisplayTexture* cover_texture = GetGameListCover(entry);
      if (entry->code.empty())
        summary.Format("%s - ", Settings::GetDiscRegionName(entry->region));
      else
        summary.Format("%s - %s - ", entry->code.c_str(), Settings::GetDiscRegionName(entry->region));

      summary.AppendString(FileSystem::GetFileNameFromPath(entry->path));

      ImGui::GetWindowDrawList()->AddImage(cover_texture->GetHandle(), bb.Min, bb.Min + image_size, ImVec2(0.0f, 0.0f),
                                           ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));

      const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
      const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
      const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
      const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, entry->title.c_str(),
                               entry->title.c_str() + entry->title.size(), nullptr, ImVec2(0.0f, 0.0f), &title_bb);
      ImGui::PopFont();

      if (summary)
      {
        ImGui::PushFont(g_medium_font);
        ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, summary, summary.GetCharArray() + summary.GetLength(),
                                 nullptr, ImVec2(0.0f, 0.0f), &summary_bb);
        ImGui::PopFont();
      }

      if (pressed)
      {
        // launch game
        const std::string& path_to_launch(entry->path);
        s_host_interface->RunLater([path_to_launch]() { DoStartPath(path_to_launch, true); });
      }

      if (hovered)
        selected_entry = entry;
    }

    EndMenuButtons();
  }
  EndFullscreenColumnWindow();

  if (BeginFullscreenColumnWindow(0.0f, 450.0f, "game_list_info", ImVec4(0.11f, 0.15f, 0.17f, 1.00f)))
  {
    ImGui::SetCursorPos(LayoutScale(ImVec2(50.0f, 50.0f)));
    ImGui::Image(selected_entry ? GetGameListCover(selected_entry)->GetHandle() :
                                  GetTextureForGameListEntryType(GameListEntryType::Count)->GetHandle(),
                 LayoutScale(ImVec2(350.0f, 350.0f)));

    const float work_width = ImGui::GetCurrentWindow()->WorkRect.GetWidth();
    constexpr float field_margin_y = 10.0f;
    constexpr float start_x = 50.0f;
    float text_y = 425.0f;
    float text_width;
    SmallString text;

    ImGui::SetCursorPos(LayoutScale(start_x, text_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, field_margin_y));
    ImGui::BeginGroup();

    if (selected_entry)
    {
      // title
      ImGui::PushFont(g_large_font);
      text_width = ImGui::CalcTextSize(selected_entry->title.c_str(), nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%s", selected_entry->title.c_str());
      ImGui::PopFont();

      ImGui::PushFont(g_medium_font);

      // developer
      if (!selected_entry->developer.empty())
      {
        text_width = ImGui::CalcTextSize(selected_entry->developer.c_str(), nullptr, false, work_width).x;
        ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
        ImGui::TextWrapped("%s", selected_entry->developer.c_str());
      }

      // code
      text_width = ImGui::CalcTextSize(selected_entry->code.c_str(), nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%s", selected_entry->code.c_str());
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 15.0f);

      // region
      ImGui::TextUnformatted("区域: ");
      ImGui::SameLine();
      ImGui::Image(s_disc_region_textures[static_cast<u32>(selected_entry->region)]->GetHandle(),
                   LayoutScale(23.0f, 16.0f));
      ImGui::SameLine();
      ImGui::Text(" (%s)", Settings::GetDiscRegionDisplayName(selected_entry->region));

      // genre
      ImGui::Text("类型: %s", selected_entry->genre.c_str());

      // release date
      char release_date_str[64];
      selected_entry->GetReleaseDateString(release_date_str, sizeof(release_date_str));
      ImGui::Text("发布日期: %s", release_date_str);

      // compatibility
      ImGui::TextUnformatted("兼容性: ");
      ImGui::SameLine();
      ImGui::Image(s_game_compatibility_textures[static_cast<u32>(selected_entry->compatibility_rating)]->GetHandle(),
                   LayoutScale(64.0f, 16.0f));
      ImGui::SameLine();
      ImGui::Text(" (%s)", GameList::GetGameListCompatibilityRatingString(selected_entry->compatibility_rating));

      // size
      ImGui::Text("大小: %.2f MB", static_cast<float>(selected_entry->total_size) / 1048576.0f);

      // game settings
      const u32 user_setting_count = selected_entry->settings.GetUserSettingsCount();
      if (user_setting_count > 0)
        ImGui::Text("%u 每游戏设置集", user_setting_count);
      else
        ImGui::TextUnformatted("未设置每游戏设置");

      ImGui::PopFont();
    }
    else
    {
      // title
      const char* title = "未选择游戏";
      ImGui::PushFont(g_large_font);
      text_width = ImGui::CalcTextSize(title, nullptr, false, work_width).x;
      ImGui::SetCursorPosX((work_width - text_width) / 2.0f);
      ImGui::TextWrapped("%s", title);
      ImGui::PopFont();
    }

    ImGui::EndGroup();
    ImGui::PopStyleVar();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - LayoutScale(50.0f));
    BeginMenuButtons();
    if (ActiveButton(ICON_FA_BACKWARD "  返回", false))
      ReturnToMainWindow();
    EndMenuButtons();
  }
  EndFullscreenColumnWindow();

  EndFullscreenColumns();
}

void EnsureGameListLoaded()
{
  // not worth using a condvar here
  if (s_game_list_load_thread.joinable())
    s_game_list_load_thread.join();

  if (s_game_list_sorted_entries.empty())
    SortGameList();
}

static void GameListRefreshThread()
{
  ProgressCallback cb("game_list_refresh");
  s_host_interface->GetGameList()->Refresh(false, false, &cb);
}

void QueueGameListRefresh()
{
  if (s_game_list_load_thread.joinable())
    s_game_list_load_thread.join();

  s_game_list_sorted_entries.clear();
  s_host_interface->GetGameList()->SetSearchDirectoriesFromSettings(*s_host_interface->GetSettingsInterface());
  s_game_list_load_thread = std::thread(GameListRefreshThread);
}

void SwitchToGameList()
{
  EnsureGameListLoaded();
  s_current_main_window = MainWindowType::GameList;
}

void SortGameList()
{
  s_game_list_sorted_entries.clear();

  for (const GameListEntry& entry : s_host_interface->GetGameList()->GetEntries())
    s_game_list_sorted_entries.push_back(&entry);

  // TODO: Custom sort types
  std::sort(s_game_list_sorted_entries.begin(), s_game_list_sorted_entries.end(),
            [](const GameListEntry* lhs, const GameListEntry* rhs) { return lhs->title < rhs->title; });
}

HostDisplayTexture* GetGameListCover(const GameListEntry* entry)
{
  // lookup and grab cover image
  auto cover_it = s_cover_image_map.find(entry->path);
  if (cover_it == s_cover_image_map.end())
  {
    std::string cover_path(s_host_interface->GetGameList()->GetCoverImagePathForEntry(entry));
    cover_it = s_cover_image_map.emplace(entry->path, std::move(cover_path)).first;
  }

  if (!cover_it->second.empty())
    return GetCachedTexture(cover_it->second);

  return GetTextureForGameListEntryType(entry->type);
}

HostDisplayTexture* GetTextureForGameListEntryType(GameListEntryType type)
{
  switch (type)
  {
    case GameListEntryType::PSExe:
      return s_fallback_exe_texture.get();

    case GameListEntryType::Playlist:
      return s_fallback_playlist_texture.get();

    case GameListEntryType::PSF:
      return s_fallback_psf_texture.get();
      break;

    case GameListEntryType::Disc:
    default:
      return s_fallback_disc_texture.get();
  }
}

HostDisplayTexture* GetCoverForCurrentGame()
{
  EnsureGameListLoaded();

  const GameListEntry* entry = s_host_interface->GetGameList()->GetEntryForPath(System::GetRunningPath().c_str());
  if (!entry)
    return s_fallback_disc_texture.get();

  return GetGameListCover(entry);
}

//////////////////////////////////////////////////////////////////////////
// Overlays
//////////////////////////////////////////////////////////////////////////

void OpenAboutWindow()
{
  s_about_window_open = true;
}

void DrawAboutWindow()
{
  ImGui::SetNextWindowSize(LayoutScale(1000.0f, 500.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup("关于DuckStation");

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(10.0f, 10.0f));

  if (ImGui::BeginPopupModal("关于DuckStation", &s_about_window_open,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    ImGui::TextWrapped("DuckStation是Sony PlayStation（TM）控制台的免费开源模拟器/仿真器, "
                       "注重可玩性、速度和长期可维护性.");
    ImGui::NewLine();
    ImGui::TextWrapped("参与者列表: https://github.com/stenzek/duckstation/blob/master/CONTRIBUTORS.md");
    ImGui::NewLine();
    ImGui::TextWrapped("鸭子图标8 (https://icons8.com/icon/74847/platforms.undefined.short-title)");
    ImGui::NewLine();
    ImGui::TextWrapped("\"PlayStation\" and \"PSX\" are registered trademarks of Sony Interactive Entertainment Europe "
                       "Limited. This software is not affiliated in any way with Sony Interactive Entertainment.");

    ImGui::NewLine();

    BeginMenuButtons();
    if (ActiveButton(ICON_FA_GLOBE "  GitHub Repository", false))
    {
      s_host_interface->RunLater(
        []() { s_host_interface->ReportError("Go to https://github.com/stenzek/duckstation/"); });
    }
    if (ActiveButton(ICON_FA_BUG "  Issue Tracker", false))
    {
      s_host_interface->RunLater(
        []() { s_host_interface->ReportError("Go to https://github.com/stenzek/duckstation/issues"); });
    }
    if (ActiveButton(ICON_FA_COMMENT "  Discord Server", false))
    {
      s_host_interface->RunLater([]() { s_host_interface->ReportError("Go to https://discord.gg/Buktv3t"); });
    }

    if (ActiveButton(ICON_FA_WINDOW_CLOSE "  关闭", false))
    {
      ImGui::CloseCurrentPopup();
      s_about_window_open = false;
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopFont();
}

bool DrawErrorWindow(const char* message)
{
  bool is_open = true;

  ImGuiFullscreen::BeginLayout();

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup("报告错误");

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(10.0f, 10.0f));

  if (ImGui::BeginPopupModal("报告错误", &is_open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    ImGui::SetCursorPos(LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
    ImGui::TextWrapped("%s", message);
    ImGui::GetCurrentWindow()->DC.CursorPos.y += LayoutScale(5.0f);

    BeginMenuButtons();

    if (ActiveButton(ICON_FA_WINDOW_CLOSE "  关闭", false))
    {
      ImGui::CloseCurrentPopup();
      is_open = false;
    }
    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopFont();

  ImGuiFullscreen::EndLayout();
  return !is_open;
}

bool DrawConfirmWindow(const char* message, bool* result)
{
  bool is_open = true;

  ImGuiFullscreen::BeginLayout();

  ImGui::SetNextWindowSize(LayoutScale(500.0f, 0.0f));
  ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::OpenPopup("确认消息");

  ImGui::PushFont(g_large_font);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(10.0f, 10.0f));

  if (ImGui::BeginPopupModal("确认消息", &is_open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize))
  {
    ImGui::SetCursorPos(LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
    ImGui::TextWrapped("%s", message);
    ImGui::GetCurrentWindow()->DC.CursorPos.y += LayoutScale(5.0f);

    BeginMenuButtons();

    bool done = false;

    if (ActiveButton(ICON_FA_CHECK "  Yes", false))
    {
      *result = true;
      done = true;
    }

    if (ActiveButton(ICON_FA_TIMES "  No", false))
    {
      *result = false;
      done = true;
    }
    if (done)
    {
      ImGui::CloseCurrentPopup();
      is_open = false;
    }

    EndMenuButtons();

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopFont();

  ImGuiFullscreen::EndLayout();
  return !is_open;
}

//////////////////////////////////////////////////////////////////////////
// Debug Menu
//////////////////////////////////////////////////////////////////////////

void SetDebugMenuAllowed(bool allowed)
{
  s_debug_menu_allowed = allowed;
  UpdateDebugMenuVisibility();
}

void SetDebugMenuEnabled(bool enabled)
{
  s_host_interface->GetSettingsInterface()->SetBoolValue("Main", "ShowDebugMenu", enabled);
  s_host_interface->GetSettingsInterface()->Save();
  UpdateDebugMenuVisibility();
}

void UpdateDebugMenuVisibility()
{
  const bool enabled =
    s_debug_menu_allowed && s_host_interface->GetSettingsInterface()->GetBoolValue("Main", "ShowDebugMenu", false);
  if (s_debug_menu_enabled == enabled)
    return;

  const float size = enabled ? DPIScale(LAYOUT_MAIN_MENU_BAR_SIZE) : 0.0f;
  s_host_interface->GetDisplay()->SetDisplayTopMargin(static_cast<s32>(size));
  ImGuiFullscreen::SetMenuBarSize(size);
  ImGuiFullscreen::UpdateLayoutScale();
  if (ImGuiFullscreen::UpdateFonts())
    s_host_interface->GetDisplay()->UpdateImGuiFontTexture();
  s_debug_menu_enabled = enabled;
}

static void DrawDebugStats();
static void DrawDebugSystemMenu();
static void DrawDebugSettingsMenu();
static void DrawDebugDebugMenu();

void DrawDebugMenu()
{
  if (!ImGui::BeginMainMenuBar())
    return;

  if (ImGui::BeginMenu("系统"))
  {
    DrawDebugSystemMenu();
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("设置"))
  {
    DrawDebugSettingsMenu();
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Debug"))
  {
    DrawDebugDebugMenu();
    ImGui::EndMenu();
  }

  DrawDebugStats();

  ImGui::EndMainMenuBar();
}

void DrawDebugStats()
{
  if (!System::IsShutdown())
  {
    const float framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale.x;
    const float framebuffer_width = ImGui::GetIO().DisplaySize.x;

    if (System::IsPaused())
    {
      ImGui::SetCursorPosX(framebuffer_width - (50.0f * framebuffer_scale));
      ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "已暂停");
    }
    else
    {
      const auto [display_width, display_height] = g_gpu->GetEffectiveDisplayResolution();
      ImGui::SetCursorPosX(framebuffer_width - (580.0f * framebuffer_scale));
      ImGui::Text("%ux%u (%s)", display_width, display_height,
                  g_gpu->IsInterlacedDisplayEnabled() ? "交错" : "进步");

      ImGui::SetCursorPosX(framebuffer_width - (420.0f * framebuffer_scale));
      ImGui::Text("平均: %.2fms", System::GetAverageFrameTime());

      ImGui::SetCursorPosX(framebuffer_width - (310.0f * framebuffer_scale));
      ImGui::Text("最差的: %.2fms", System::GetWorstFrameTime());

      ImGui::SetCursorPosX(framebuffer_width - (210.0f * framebuffer_scale));

      const float speed = System::GetEmulationSpeed();
      const u32 rounded_speed = static_cast<u32>(std::round(speed));
      if (speed < 90.0f)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%u%%", rounded_speed);
      else if (speed < 110.0f)
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%u%%", rounded_speed);
      else
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%u%%", rounded_speed);

      ImGui::SetCursorPosX(framebuffer_width - (165.0f * framebuffer_scale));
      ImGui::Text("FPS: %.2f", System::GetFPS());

      ImGui::SetCursorPosX(framebuffer_width - (80.0f * framebuffer_scale));
      ImGui::Text("VPS: %.2f", System::GetVPS());
    }
  }
}

void DrawDebugSystemMenu()
{
  const bool system_enabled = static_cast<bool>(!System::IsShutdown());

  if (ImGui::MenuItem("启动光盘", nullptr, false, !system_enabled))
  {
    DoStartFile();
    ClearImGuiFocus();
  }

  if (ImGui::MenuItem("启动BIOS", nullptr, false, !system_enabled))
  {
    DoStartBIOS();
    ClearImGuiFocus();
  }

  ImGui::Separator();

  if (ImGui::MenuItem("关闭电源", nullptr, false, system_enabled))
  {
    DoPowerOff();
    ClearImGuiFocus();
  }

  if (ImGui::MenuItem("重置", nullptr, false, system_enabled))
  {
    DoReset();
    ClearImGuiFocus();
  }

  if (ImGui::MenuItem("暂停", nullptr, System::IsPaused(), system_enabled))
  {
    DoPause();
    ClearImGuiFocus();
  }

  ImGui::Separator();

  if (ImGui::MenuItem("更改光盘", nullptr, false, system_enabled))
  {
    DoChangeDisc();
    ClearImGuiFocus();
  }

  if (ImGui::MenuItem("删除光盘", nullptr, false, system_enabled))
  {
    s_host_interface->RunLater([]() { System::RemoveMedia(); });
    ClearImGuiFocus();
  }

  ImGui::Separator();

  if (ImGui::BeginMenu("加载状态", !IsCheevosHardcoreModeActive()))
  {
    for (u32 i = 1; i <= CommonHostInterface::GLOBAL_SAVE_STATE_SLOTS; i++)
    {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "状态 %u", i);
      if (ImGui::MenuItem(buf))
      {
        s_host_interface->RunLater([i]() { s_host_interface->LoadState(true, i); });
        ClearImGuiFocus();
      }
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("保存状态", system_enabled))
  {
    for (u32 i = 1; i <= CommonHostInterface::GLOBAL_SAVE_STATE_SLOTS; i++)
    {
      TinyString buf;
      buf.Format("状态 %u", i);
      if (ImGui::MenuItem(buf))
      {
        s_host_interface->RunLater([i]() { s_host_interface->SaveState(true, i); });
        ClearImGuiFocus();
      }
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();

  if (ImGui::BeginMenu("作弊", system_enabled && !IsCheevosHardcoreModeActive()))
  {
    const bool has_cheat_file = System::HasCheatList();
    if (ImGui::BeginMenu("启用作弊", has_cheat_file))
    {
      CheatList* cl = System::GetCheatList();
      for (u32 i = 0; i < cl->GetCodeCount(); i++)
      {
        const CheatCode& cc = cl->GetCode(i);
        if (ImGui::MenuItem(cc.description.c_str(), nullptr, cc.enabled, true))
          s_host_interface->SetCheatCodeState(i, !cc.enabled, g_settings.auto_load_cheats);
      }

      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("应用作弊", has_cheat_file))
    {
      CheatList* cl = System::GetCheatList();
      for (u32 i = 0; i < cl->GetCodeCount(); i++)
      {
        const CheatCode& cc = cl->GetCode(i);
        if (ImGui::MenuItem(cc.description.c_str()))
          s_host_interface->ApplyCheatCode(i);
      }

      ImGui::EndMenu();
    }

    ImGui::EndMenu();
  }

  ImGui::Separator();

  if (ImGui::MenuItem("退出"))
    s_host_interface->RequestExit();
}

void DrawDebugSettingsMenu()
{
  bool settings_changed = false;

  if (ImGui::BeginMenu("CPU执行模式"))
  {
    const CPUExecutionMode current = s_settings_copy.cpu_execution_mode;
    for (u32 i = 0; i < static_cast<u32>(CPUExecutionMode::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetCPUExecutionModeDisplayName(static_cast<CPUExecutionMode>(i)), nullptr,
                          i == static_cast<u32>(current)))
      {
        s_settings_copy.cpu_execution_mode = static_cast<CPUExecutionMode>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  if (ImGui::MenuItem("CPU时钟控制", nullptr, &s_settings_copy.cpu_overclock_enable))
  {
    settings_changed = true;
    s_settings_copy.UpdateOverclockActive();
  }

  if (ImGui::BeginMenu("CPU时钟速度"))
  {
    static constexpr auto values = make_array(10u, 25u, 50u, 75u, 100u, 125u, 150u, 175u, 200u, 225u, 250u, 275u, 300u,
                                              350u, 400u, 450u, 500u, 600u, 700u, 800u);
    const u32 percent = s_settings_copy.GetCPUOverclockPercent();
    for (u32 value : values)
    {
      if (ImGui::MenuItem(TinyString::FromFormat("%u%%", value), nullptr, percent == value))
      {
        s_settings_copy.SetCPUOverclockPercent(value);
        s_settings_copy.UpdateOverclockActive();
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  settings_changed |=
    ImGui::MenuItem("重编译器内存异常", nullptr, &s_settings_copy.cpu_recompiler_memory_exceptions);
  settings_changed |=
    ImGui::MenuItem("重新编译块链接", nullptr, &s_settings_copy.cpu_recompiler_block_linking);
  if (ImGui::BeginMenu("重组器Fastmem"))
  {
    for (u32 i = 0; i < static_cast<u32>(CPUFastmemMode::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetCPUFastmemModeDisplayName(static_cast<CPUFastmemMode>(i)), nullptr,
                          s_settings_copy.cpu_fastmem_mode == static_cast<CPUFastmemMode>(i)))
      {
        s_settings_copy.cpu_fastmem_mode = static_cast<CPUFastmemMode>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("重新编译iCache", nullptr, &s_settings_copy.cpu_recompiler_icache);

  ImGui::Separator();

  if (ImGui::BeginMenu("渲染器"))
  {
    const GPURenderer current = s_settings_copy.gpu_renderer;
    for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetRendererDisplayName(static_cast<GPURenderer>(i)), nullptr,
                          i == static_cast<u32>(current)))
      {
        s_settings_copy.gpu_renderer = static_cast<GPURenderer>(i);
        settings_changed = true;
      }
    }

    settings_changed |= ImGui::MenuItem("线程上的GPU", nullptr, &s_settings_copy.gpu_use_thread);

    ImGui::EndMenu();
  }

  if (ImGui::MenuItem("切换全屏"))
    s_host_interface->RunLater([] { s_host_interface->SetFullscreen(!s_host_interface->IsFullscreen()); });

  if (ImGui::BeginMenu("根据游戏调整大小", System::IsValid()))
  {
    static constexpr auto scales = make_array(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    for (const u32 scale : scales)
    {
      if (ImGui::MenuItem(TinyString::FromFormat("%ux 规模", scale)))
        s_host_interface->RunLater(
          [scale]() { s_host_interface->RequestRenderWindowScale(static_cast<float>(scale)); });
    }

    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("VSync", nullptr, &s_settings_copy.video_sync_enabled);

  ImGui::Separator();

  if (ImGui::BeginMenu("分辨率比例"))
  {
    const u32 current_internal_resolution = s_settings_copy.gpu_resolution_scale;
    for (u32 scale = 1; scale <= GPU::MAX_RESOLUTION_SCALE; scale++)
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%ux (%ux%u)", scale, scale * VRAM_WIDTH, scale * VRAM_HEIGHT);

      if (ImGui::MenuItem(buf, nullptr, current_internal_resolution == scale))
      {
        s_settings_copy.gpu_resolution_scale = scale;
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("多重采样"))
  {
    const u32 current_multisamples = s_settings_copy.gpu_multisamples;
    const bool current_ssaa = s_settings_copy.gpu_per_sample_shading;

    if (ImGui::MenuItem("没有", nullptr, (current_multisamples == 1)))
    {
      s_settings_copy.gpu_multisamples = 1;
      s_settings_copy.gpu_per_sample_shading = false;
      settings_changed = true;
    }

    for (u32 i = 2; i <= 32; i *= 2)
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%ux MSAA", i);

      if (ImGui::MenuItem(buf, nullptr, (current_multisamples == i && !current_ssaa)))
      {
        s_settings_copy.gpu_multisamples = i;
        s_settings_copy.gpu_per_sample_shading = false;
        settings_changed = true;
      }
    }

    for (u32 i = 2; i <= 32; i *= 2)
    {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%ux SSAA", i);

      if (ImGui::MenuItem(buf, nullptr, (current_multisamples == i && current_ssaa)))
      {
        s_settings_copy.gpu_multisamples = i;
        s_settings_copy.gpu_per_sample_shading = true;
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("PGXP"))
  {
    settings_changed |= ImGui::MenuItem("PGXP 启用", nullptr, &s_settings_copy.gpu_pgxp_enable);
    settings_changed |=
      ImGui::MenuItem("PGXP 消隐", nullptr, &s_settings_copy.gpu_pgxp_culling, s_settings_copy.gpu_pgxp_enable);
    settings_changed |= ImGui::MenuItem("PGXP 纹理校正", nullptr,
                                        &s_settings_copy.gpu_pgxp_texture_correction, s_settings_copy.gpu_pgxp_enable);
    settings_changed |= ImGui::MenuItem("PGXP 顶点缓存", nullptr, &s_settings_copy.gpu_pgxp_vertex_cache,
                                        s_settings_copy.gpu_pgxp_enable);
    settings_changed |=
      ImGui::MenuItem("PGXP CPU 说明书", nullptr, &s_settings_copy.gpu_pgxp_cpu, s_settings_copy.gpu_pgxp_enable);
    settings_changed |= ImGui::MenuItem("PGXP 保持投影精度", nullptr,
                                        &s_settings_copy.gpu_pgxp_preserve_proj_fp, s_settings_copy.gpu_pgxp_enable);
    settings_changed |= ImGui::MenuItem("PGXP 深度缓冲", nullptr, &s_settings_copy.gpu_pgxp_depth_buffer,
                                        s_settings_copy.gpu_pgxp_enable);
    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("真（24位）颜色", nullptr, &s_settings_copy.gpu_true_color);
  settings_changed |= ImGui::MenuItem("缩放抖动", nullptr, &s_settings_copy.gpu_scaled_dithering);

  if (ImGui::BeginMenu("纹理过滤"))
  {
    const GPUTextureFilter current = s_settings_copy.gpu_texture_filter;
    for (u32 i = 0; i < static_cast<u32>(GPUTextureFilter::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetTextureFilterDisplayName(static_cast<GPUTextureFilter>(i)), nullptr,
                          i == static_cast<u32>(current)))
      {
        s_settings_copy.gpu_texture_filter = static_cast<GPUTextureFilter>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("禁用隔行扫描", nullptr, &s_settings_copy.gpu_disable_interlacing);
  settings_changed |= ImGui::MenuItem("宽屏黑客", nullptr, &s_settings_copy.gpu_widescreen_hack);
  settings_changed |= ImGui::MenuItem("强制NTSC计时", nullptr, &s_settings_copy.gpu_force_ntsc_timings);
  settings_changed |= ImGui::MenuItem("24位色度平滑", nullptr, &s_settings_copy.gpu_24bit_chroma_smoothing);

  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("显示线性过滤", nullptr, &s_settings_copy.display_linear_filtering);
  settings_changed |= ImGui::MenuItem("显示整数缩放", nullptr, &s_settings_copy.display_integer_scaling);

  if (ImGui::BeginMenu("纵横比"))
  {
    for (u32 i = 0; i < static_cast<u32>(DisplayAspectRatio::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetDisplayAspectRatioName(static_cast<DisplayAspectRatio>(i)), nullptr,
                          s_settings_copy.display_aspect_ratio == static_cast<DisplayAspectRatio>(i)))
      {
        s_settings_copy.display_aspect_ratio = static_cast<DisplayAspectRatio>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("裁剪模式"))
  {
    for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(i)), nullptr,
                          s_settings_copy.display_crop_mode == static_cast<DisplayCropMode>(i)))
      {
        s_settings_copy.display_crop_mode = static_cast<DisplayCropMode>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("降采样模式"))
  {
    for (u32 i = 0; i < static_cast<u32>(GPUDownsampleMode::Count); i++)
    {
      if (ImGui::MenuItem(Settings::GetDownsampleModeDisplayName(static_cast<GPUDownsampleMode>(i)), nullptr,
                          s_settings_copy.gpu_downsample_mode == static_cast<GPUDownsampleMode>(i)))
      {
        s_settings_copy.gpu_downsample_mode = static_cast<GPUDownsampleMode>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("24位强制4:3", nullptr, &s_settings_copy.display_force_4_3_for_24bit);

  ImGui::Separator();

  if (ImGui::MenuItem("转储音频", nullptr, s_host_interface->IsDumpingAudio(), System::IsValid()))
  {
    if (!s_host_interface->IsDumpingAudio())
      s_host_interface->StartDumpingAudio();
    else
      s_host_interface->StopDumpingAudio();
  }

  if (ImGui::MenuItem("保存截图"))
    s_host_interface->RunLater([]() { s_host_interface->SaveScreenshot(); });

  if (settings_changed)
    s_host_interface->RunLater(SaveAndApplySettings);
}

void DrawDebugDebugMenu()
{
  const bool system_valid = System::IsValid();
  Settings::DebugSettings& debug_settings = g_settings.debugging;
  bool settings_changed = false;

  if (ImGui::BeginMenu("日志级别"))
  {
    for (u32 i = LOGLEVEL_NONE; i < LOGLEVEL_COUNT; i++)
    {
      if (ImGui::MenuItem(Settings::GetLogLevelDisplayName(static_cast<LOGLEVEL>(i)), nullptr,
                          g_settings.log_level == static_cast<LOGLEVEL>(i)))
      {
        s_settings_copy.log_level = static_cast<LOGLEVEL>(i);
        settings_changed = true;
      }
    }

    ImGui::EndMenu();
  }

  settings_changed |= ImGui::MenuItem("Log To Console", nullptr, &s_settings_copy.log_to_console);
  settings_changed |= ImGui::MenuItem("Log To Debug", nullptr, &s_settings_copy.log_to_debug);
  settings_changed |= ImGui::MenuItem("Log To File", nullptr, &s_settings_copy.log_to_file);

  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("禁用所有增强功能", nullptr, &s_settings_copy.disable_all_enhancements);
  settings_changed |= ImGui::MenuItem("将CPU转储到VRAM副本", nullptr, &debug_settings.dump_cpu_to_vram_copies);
  settings_changed |= ImGui::MenuItem("将VRAM转储到CPU副本", nullptr, &debug_settings.dump_vram_to_cpu_copies);

  if (ImGui::MenuItem("CPU跟踪日志", nullptr, CPU::IsTraceEnabled(), system_valid))
  {
    if (!CPU::IsTraceEnabled())
      CPU::StartTrace();
    else
      CPU::StopTrace();
  }

  ImGui::Separator();

  settings_changed |= ImGui::MenuItem("显示VRAM", nullptr, &debug_settings.show_vram);
  settings_changed |= ImGui::MenuItem("显示GPU状态", nullptr, &debug_settings.show_gpu_state);
  settings_changed |= ImGui::MenuItem("显示CDROM状态", nullptr, &debug_settings.show_cdrom_state);
  settings_changed |= ImGui::MenuItem("显示SPU状态", nullptr, &debug_settings.show_spu_state);
  settings_changed |= ImGui::MenuItem("显示计时器状态", nullptr, &debug_settings.show_timers_state);
  settings_changed |= ImGui::MenuItem("显示MDEC状态", nullptr, &debug_settings.show_mdec_state);
  settings_changed |= ImGui::MenuItem("显示DMA状态", nullptr, &debug_settings.show_dma_state);

  if (settings_changed)
  {
    // have to apply it to the copy too, otherwise it won't save
    Settings::DebugSettings& debug_settings_copy = s_settings_copy.debugging;
    debug_settings_copy.show_gpu_state = debug_settings.show_gpu_state;
    debug_settings_copy.show_vram = debug_settings.show_vram;
    debug_settings_copy.dump_cpu_to_vram_copies = debug_settings.dump_cpu_to_vram_copies;
    debug_settings_copy.dump_vram_to_cpu_copies = debug_settings.dump_vram_to_cpu_copies;
    debug_settings_copy.show_cdrom_state = debug_settings.show_cdrom_state;
    debug_settings_copy.show_spu_state = debug_settings.show_spu_state;
    debug_settings_copy.show_timers_state = debug_settings.show_timers_state;
    debug_settings_copy.show_mdec_state = debug_settings.show_mdec_state;
    debug_settings_copy.show_dma_state = debug_settings.show_dma_state;
    s_host_interface->RunLater(SaveAndApplySettings);
  }
}

#ifdef WITH_CHEEVOS

static void DrawAchievement(const Cheevos::Achievement& cheevo)
{
  static constexpr float alpha = 0.8f;
  static constexpr float progress_height_unscaled = 20.0f;
  static constexpr float progress_spacing_unscaled = 5.0f;

  TinyString id_str;
  id_str.Format("%u", cheevo.id);

  const auto progress = Cheevos::GetAchievementProgress(cheevo);
  const bool is_measured = progress.second != 0;

  ImRect bb;
  bool visible, hovered;
  bool pressed =
    MenuButtonFrame(id_str, true,
                    !is_measured ? LAYOUT_MENU_BUTTON_HEIGHT :
                                   LAYOUT_MENU_BUTTON_HEIGHT + progress_height_unscaled + progress_spacing_unscaled,
                    &visible, &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const ImVec2 image_size(LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT, LAYOUT_MENU_BUTTON_HEIGHT));
  const std::string& badge_path = cheevo.locked ? cheevo.locked_badge_path : cheevo.unlocked_badge_path;
  if (!badge_path.empty())
  {
    HostDisplayTexture* badge = GetCachedTexture(badge_path);
    if (badge)
    {
      ImGui::GetWindowDrawList()->AddImage(badge->GetHandle(), bb.Min, bb.Min + image_size, ImVec2(0.0f, 0.0f),
                                           ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
    }
  }

  const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
  const float text_start_x = bb.Min.x + image_size.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

  ImGui::PushFont(g_large_font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, cheevo.title.c_str(), cheevo.title.c_str() + cheevo.title.size(),
                           nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  if (!cheevo.description.empty())
  {
    ImGui::PushFont(g_medium_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, cheevo.description.c_str(),
                             cheevo.description.c_str() + cheevo.description.size(), nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (is_measured)
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float progress_height = LayoutScale(progress_height_unscaled);
    const float progress_spacing = LayoutScale(progress_spacing_unscaled);
    const float top = midpoint + g_medium_font->FontSize + progress_spacing;
    const ImRect progress_bb(ImVec2(text_start_x, top), ImVec2(bb.Max.x, top + progress_height));
    const float fraction = static_cast<float>(progress.first) / static_cast<float>(progress.second);
    dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryDarkColor()));
    dl->AddRectFilled(progress_bb.Min, ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                      ImGui::GetColorU32(ImGuiFullscreen::UISecondaryColor()));

    const TinyString text(GetAchievementProgressText(cheevo));
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                          progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) - (text_size.y / 2.0f));
    dl->AddText(g_medium_font, g_medium_font->FontSize, text_pos,
                ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryTextColor()), text.GetCharArray(),
                text.GetCharArray() + text.GetLength());
  }

#if 0
  // The API doesn't seem to send us this :(
  if (!cheevo.locked)
  {
    ImGui::PushFont(g_medium_font);

    const ImRect time_bb(ImVec2(text_start_x, bb.Min.y),
      ImVec2(bb.Max.x, bb.Min.y + g_medium_font->FontSize + LayoutScale(4.0f)));
    text.Format("Unlocked 21 Feb, 2019 @ 3:14am");
    ImGui::RenderTextClipped(time_bb.Min, time_bb.Max, text.GetCharArray(), text.GetCharArray() + text.GetLength(),
      nullptr, ImVec2(1.0f, 0.0f), &time_bb);
    ImGui::PopFont();
  }
#endif

  if (pressed)
  {
    // TODO: What should we do here?
    // Display information or something..
  }
}

void DrawAchievementWindow()
{
  static constexpr float alpha = 0.8f;
  static constexpr float heading_height_unscaled = 110.0f;

  ImGui::SetNextWindowBgAlpha(alpha);

  const ImVec4 background(0.13f, 0.13f, 0.13f, alpha);
  const ImVec2 display_size(ImGui::GetIO().DisplaySize);
  const float heading_height = LayoutScale(heading_height_unscaled);

  if (BeginFullscreenWindow(
        ImVec2(0.0f, 0.0f), ImVec2(display_size.x, heading_height), "achievements_heading", background, 0.0f, 0.0f,
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse))
  {
    ImRect bb;
    bool visible, hovered;
    bool pressed = MenuButtonFrame("achievements_heading", false, heading_height_unscaled, &visible, &hovered, &bb.Min,
                                   &bb.Max, 0, alpha);
    UNREFERENCED_VARIABLE(pressed);

    if (visible)
    {
      const float padding = LayoutScale(10.0f);
      const float spacing = LayoutScale(10.0f);
      const float image_height = LayoutScale(85.0f);

      const ImVec2 icon_min(bb.Min + ImVec2(padding, padding));
      const ImVec2 icon_max(icon_min + ImVec2(image_height, image_height));

      const std::string& icon_path = Cheevos::GetGameIcon();
      if (!icon_path.empty())
      {
        HostDisplayTexture* badge = GetCachedTexture(icon_path);
        if (badge)
        {
          ImGui::GetWindowDrawList()->AddImage(badge->GetHandle(), icon_min, icon_max, ImVec2(0.0f, 0.0f),
                                               ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
        }
      }

      float left = bb.Min.x + padding + image_height + spacing;
      float right = bb.Max.x - padding;
      float top = bb.Min.y + padding;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      SmallString text;
      ImVec2 text_size;

      const u32 unlocked_count = Cheevos::GetUnlockedAchiementCount();
      const u32 achievement_count = Cheevos::GetAchievementCount();
      const u32 current_points = Cheevos::GetCurrentPointsForGame();
      const u32 total_points = Cheevos::GetMaximumPointsForGame();

      if (FloatingButton(ICON_FA_WINDOW_CLOSE, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true, g_large_font) ||
          WantsToCloseMenu())
      {
        ReturnToMainWindow();
      }

      const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + g_large_font->FontSize));
      text.Assign(Cheevos::GetGameTitle());

      if (Cheevos::IsChallengeModeActive())
        text.AppendString(" (硬核模式)");

      top += g_large_font->FontSize + spacing;

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, text.GetCharArray(), text.GetCharArray() + text.GetLength(),
                               nullptr, ImVec2(0.0f, 0.0f), &title_bb);
      ImGui::PopFont();

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
      if (unlocked_count == achievement_count)
      {
        text.Format("您已解锁所有成就并获得 %u 分！", total_points);
      }
      else
      {
        text.Format("You have unlocked %u of %u achievements, earning %u of %u possible points.", unlocked_count,
                    achievement_count, current_points, total_points);
      }

      top += g_medium_font->FontSize + spacing;

      ImGui::PushFont(g_medium_font);
      ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, text.GetCharArray(),
                               text.GetCharArray() + text.GetLength(), nullptr, ImVec2(0.0f, 0.0f), &summary_bb);
      ImGui::PopFont();

      const float progress_height = LayoutScale(20.0f);
      const ImRect progress_bb(ImVec2(left, top), ImVec2(right, top + progress_height));
      const float fraction = static_cast<float>(unlocked_count) / static_cast<float>(achievement_count);
      dl->AddRectFilled(progress_bb.Min, progress_bb.Max, ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryDarkColor()));
      dl->AddRectFilled(progress_bb.Min,
                        ImVec2(progress_bb.Min.x + fraction * progress_bb.GetWidth(), progress_bb.Max.y),
                        ImGui::GetColorU32(ImGuiFullscreen::UISecondaryColor()));

      text.Format("%d%%", static_cast<int>(std::round(fraction * 100.0f)));
      text_size = ImGui::CalcTextSize(text);
      const ImVec2 text_pos(progress_bb.Min.x + ((progress_bb.Max.x - progress_bb.Min.x) / 2.0f) - (text_size.x / 2.0f),
                            progress_bb.Min.y + ((progress_bb.Max.y - progress_bb.Min.y) / 2.0f) -
                              (text_size.y / 2.0f));
      dl->AddText(g_medium_font, g_medium_font->FontSize, text_pos,
                  ImGui::GetColorU32(ImGuiFullscreen::UIPrimaryTextColor()), text.GetCharArray(),
                  text.GetCharArray() + text.GetLength());
      top += progress_height + spacing;
    }
  }
  EndFullscreenWindow();

  ImGui::SetNextWindowBgAlpha(alpha);

  if (BeginFullscreenWindow(ImVec2(0.0f, heading_height), ImVec2(display_size.x, display_size.y - heading_height),
                            "成就", background, 0.0f, 0.0f, 0))
  {
    BeginMenuButtons();

    static bool unlocked_achievements_collapsed = false;

    unlocked_achievements_collapsed ^= MenuHeadingButton(
      "解锁成就", unlocked_achievements_collapsed ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP);
    if (!unlocked_achievements_collapsed)
    {
      Cheevos::EnumerateAchievements([](const Cheevos::Achievement& cheevo) -> bool {
        if (!cheevo.locked)
          DrawAchievement(cheevo);

        return true;
      });
    }

    if (Cheevos::GetUnlockedAchiementCount() != Cheevos::GetAchievementCount())
    {
      static bool locked_achievements_collapsed = false;
      locked_achievements_collapsed ^= MenuHeadingButton(
        "锁定的成就", locked_achievements_collapsed ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP);
      if (!locked_achievements_collapsed)
      {
        Cheevos::EnumerateAchievements([](const Cheevos::Achievement& cheevo) -> bool {
          if (cheevo.locked)
            DrawAchievement(cheevo);

          return true;
        });
      }
    }

    EndMenuButtons();
  }
  EndFullscreenWindow();
}

static void DrawLeaderboardListEntry(const Cheevos::Leaderboard& lboard)
{
  static constexpr float alpha = 0.8f;

  TinyString id_str;
  id_str.Format("%u", lboard.id);

  ImRect bb;
  bool visible, hovered;
  bool pressed =
    MenuButtonFrame(id_str, true, LAYOUT_MENU_BUTTON_HEIGHT, &visible, &hovered, &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
  const float text_start_x = bb.Min.x + LayoutScale(15.0f);
  const ImRect title_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  const ImRect summary_bb(ImVec2(text_start_x, midpoint), bb.Max);

  ImGui::PushFont(g_large_font);
  ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, lboard.title.c_str(), lboard.title.c_str() + lboard.title.size(),
                           nullptr, ImVec2(0.0f, 0.0f), &title_bb);
  ImGui::PopFont();

  if (!lboard.description.empty())
  {
    ImGui::PushFont(g_medium_font);
    ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, lboard.description.c_str(),
                             lboard.description.c_str() + lboard.description.size(), nullptr, ImVec2(0.0f, 0.0f),
                             &summary_bb);
    ImGui::PopFont();
  }

  if (pressed)
  {
    s_open_leaderboard_id = lboard.id;
  }
}

static void DrawLeaderboardEntry(const Cheevos::LeaderboardEntry& lbEntry, float rank_column_width,
                                 float name_column_width, float column_spacing)
{
  static constexpr float alpha = 0.8f;

  ImRect bb;
  bool visible, hovered;
  bool pressed = MenuButtonFrame(lbEntry.user.c_str(), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible, &hovered,
                                 &bb.Min, &bb.Max, 0, alpha);
  if (!visible)
    return;

  const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
  float text_start_x = bb.Min.x + LayoutScale(15.0f);
  SmallString text;

  text.Format("%u", lbEntry.rank);

  ImGui::PushFont(g_large_font);
  if (lbEntry.is_self)
  {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(255, 242, 0, 255));
  }

  const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(rank_bb.Min, rank_bb.Max, text.GetCharArray(), text.GetCharArray() + text.GetLength(),
                           nullptr, ImVec2(0.0f, 0.0f), &rank_bb);
  text_start_x += rank_column_width + column_spacing;

  const ImRect user_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(user_bb.Min, user_bb.Max, lbEntry.user.c_str(), lbEntry.user.c_str() + lbEntry.user.size(),
                           nullptr, ImVec2(0.0f, 0.0f), &user_bb);
  text_start_x += name_column_width + column_spacing;

  const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
  ImGui::RenderTextClipped(score_bb.Min, score_bb.Max, lbEntry.formatted_score.c_str(),
                           lbEntry.formatted_score.c_str() + lbEntry.formatted_score.size(), nullptr,
                           ImVec2(0.0f, 0.0f), &score_bb);

  if (lbEntry.is_self)
  {
    ImGui::PopStyleColor();
  }

  ImGui::PopFont();

  // This API DOES list the submission date/time, but is it relevant?
#if 0
  if (!cheevo.locked)
  {
    ImGui::PushFont(g_medium_font);

    const ImRect time_bb(ImVec2(text_start_x, bb.Min.y),
      ImVec2(bb.Max.x, bb.Min.y + g_medium_font->FontSize + LayoutScale(4.0f)));
    text.Format("Unlocked 21 Feb, 2019 @ 3:14am");
    ImGui::RenderTextClipped(time_bb.Min, time_bb.Max, text.GetCharArray(), text.GetCharArray() + text.GetLength(),
      nullptr, ImVec2(1.0f, 0.0f), &time_bb);
    ImGui::PopFont();
  }
#endif

  if (pressed)
  {
    // Anything?
  }
}

void DrawLeaderboardsWindow()
{
  static constexpr float alpha = 0.8f;
  static constexpr float heading_height_unscaled = 110.0f;

  ImGui::SetNextWindowBgAlpha(alpha);

  const bool is_leaderboard_open = s_open_leaderboard_id.has_value();
  bool close_leaderboard_on_exit = false;

  const ImVec4 background(0.13f, 0.13f, 0.13f, alpha);
  const ImVec2 display_size(ImGui::GetIO().DisplaySize);
  const float padding = LayoutScale(10.0f);
  const float spacing = LayoutScale(10.0f);
  const float spacing_small = spacing / 2.0f;
  float heading_height = LayoutScale(heading_height_unscaled);
  if (is_leaderboard_open)
  {
    // Add space for a legend - spacing + 1 line of text + spacing + line
    heading_height += spacing + LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + spacing;
  }

  const float rank_column_width =
    g_large_font->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(), -1.0f, "99999").x;
  const float name_column_width =
    g_large_font
      ->CalcTextSizeA(g_large_font->FontSize, std::numeric_limits<float>::max(), -1.0f, "WWWWWWWWWWWWWWWWWWWW")
      .x;
  const float column_spacing = spacing * 2.0f;

  if (BeginFullscreenWindow(
        ImVec2(0.0f, 0.0f), ImVec2(display_size.x, heading_height), "leaderboards_heading", background, 0.0f, 0.0f,
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse))
  {
    ImRect bb;
    bool visible, hovered;
    bool pressed = MenuButtonFrame("leaderboards_heading", false, heading_height_unscaled, &visible, &hovered, &bb.Min,
                                   &bb.Max, 0, alpha);
    UNREFERENCED_VARIABLE(pressed);

    if (visible)
    {
      const float image_height = LayoutScale(85.0f);

      const ImVec2 icon_min(bb.Min + ImVec2(padding, padding));
      const ImVec2 icon_max(icon_min + ImVec2(image_height, image_height));

      const std::string& icon_path = Cheevos::GetGameIcon();
      if (!icon_path.empty())
      {
        HostDisplayTexture* badge = GetCachedTexture(icon_path);
        if (badge)
        {
          ImGui::GetWindowDrawList()->AddImage(badge->GetHandle(), icon_min, icon_max, ImVec2(0.0f, 0.0f),
                                               ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 255));
        }
      }

      float left = bb.Min.x + padding + image_height + spacing;
      float right = bb.Max.x - padding;
      float top = bb.Min.y + padding;
      SmallString text;
      ImVec2 text_size;

      const u32 leaderboard_count = Cheevos::GetLeaderboardCount();

      if (!is_leaderboard_open)
      {
        if (FloatingButton(ICON_FA_WINDOW_CLOSE, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true, g_large_font) ||
            WantsToCloseMenu())
        {
          ReturnToMainWindow();
        }
      }
      else
      {
        if (FloatingButton(ICON_FA_CARET_SQUARE_LEFT, 10.0f, 10.0f, -1.0f, -1.0f, 1.0f, 0.0f, true, g_large_font) ||
            WantsToCloseMenu())
        {
          close_leaderboard_on_exit = true;
        }
      }

      const ImRect title_bb(ImVec2(left, top), ImVec2(right, top + g_large_font->FontSize));
      text.Assign(Cheevos::GetGameTitle());

      top += g_large_font->FontSize + spacing;

      ImGui::PushFont(g_large_font);
      ImGui::RenderTextClipped(title_bb.Min, title_bb.Max, text.GetCharArray(), text.GetCharArray() + text.GetLength(),
                               nullptr, ImVec2(0.0f, 0.0f), &title_bb);
      ImGui::PopFont();

      if (s_open_leaderboard_id)
      {
        const Cheevos::Leaderboard* lboard = Cheevos::GetLeaderboardByID(*s_open_leaderboard_id);
        if (lboard != nullptr)
        {
          const ImRect subtitle_bb(ImVec2(left, top), ImVec2(right, top + g_large_font->FontSize));
          text.Assign(lboard->title);

          top += g_large_font->FontSize + spacing_small;

          ImGui::PushFont(g_large_font);
          ImGui::RenderTextClipped(subtitle_bb.Min, subtitle_bb.Max, text.GetCharArray(),
                                   text.GetCharArray() + text.GetLength(), nullptr, ImVec2(0.0f, 0.0f), &subtitle_bb);
          ImGui::PopFont();

          text.Assign(lboard->description);
        }
        else
        {
          text.Clear();
        }
      }
      else
      {
        text.Format("此游戏有 %u 个排行榜.", leaderboard_count);
      }

      const ImRect summary_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
      top += g_medium_font->FontSize + spacing_small;

      ImGui::PushFont(g_medium_font);
      ImGui::RenderTextClipped(summary_bb.Min, summary_bb.Max, text.GetCharArray(),
                               text.GetCharArray() + text.GetLength(), nullptr, ImVec2(0.0f, 0.0f), &summary_bb);

      if (!IsCheevosHardcoreModeActive())
      {
        const ImRect hardcore_warning_bb(ImVec2(left, top), ImVec2(right, top + g_medium_font->FontSize));
        top += g_medium_font->FontSize + spacing_small;

        ImGui::RenderTextClipped(
          hardcore_warning_bb.Min, hardcore_warning_bb.Max,
          "提交分数是禁用的，因为硬核模式是关闭的。排行榜是只读的.", nullptr, nullptr,
          ImVec2(0.0f, 0.0f), &hardcore_warning_bb);
      }

      ImGui::PopFont();
    }

    if (is_leaderboard_open)
    {
      pressed = MenuButtonFrame("legend", false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY, &visible, &hovered, &bb.Min,
                                &bb.Max, 0, alpha);

      UNREFERENCED_VARIABLE(pressed);

      if (visible)
      {
        const Cheevos::Leaderboard* lboard = Cheevos::GetLeaderboardByID(*s_open_leaderboard_id);

        const float midpoint = bb.Min.y + g_large_font->FontSize + LayoutScale(4.0f);
        float text_start_x = bb.Min.x + LayoutScale(15.0f) + padding;

        ImGui::PushFont(g_large_font);

        const ImRect rank_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(rank_bb.Min, rank_bb.Max, "Rank", nullptr, nullptr, ImVec2(0.0f, 0.0f), &rank_bb);
        text_start_x += rank_column_width + column_spacing;

        const ImRect user_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(user_bb.Min, user_bb.Max, "Name", nullptr, nullptr, ImVec2(0.0f, 0.0f), &user_bb);
        text_start_x += name_column_width + column_spacing;

        const ImRect score_bb(ImVec2(text_start_x, bb.Min.y), ImVec2(bb.Max.x, midpoint));
        ImGui::RenderTextClipped(score_bb.Min, score_bb.Max,
                                 lboard != nullptr && Cheevos::IsLeaderboardTimeType(*lboard) ? "Time" : "Score",
                                 nullptr, nullptr, ImVec2(0.0f, 0.0f), &score_bb);

        ImGui::PopFont();

        const float line_thickness = LayoutScale(1.0f);
        const float line_padding = LayoutScale(5.0f);
        const ImVec2 line_start(bb.Min.x, bb.Min.y + g_large_font->FontSize + line_padding);
        const ImVec2 line_end(bb.Max.x, line_start.y);
        ImGui::GetWindowDrawList()->AddLine(line_start, line_end, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            line_thickness);
      }
    }
  }
  EndFullscreenWindow();

  ImGui::SetNextWindowBgAlpha(alpha);

  if (!is_leaderboard_open)
  {
    if (BeginFullscreenWindow(ImVec2(0.0f, heading_height), ImVec2(display_size.x, display_size.y - heading_height),
                              "游戏排行榜", background, 0.0f, 0.0f, 0))
    {
      BeginMenuButtons();

      Cheevos::EnumerateLeaderboards([](const Cheevos::Leaderboard& lboard) -> bool {
        DrawLeaderboardListEntry(lboard);

        return true;
      });

      EndMenuButtons();
    }
    EndFullscreenWindow();
  }
  else
  {
    if (BeginFullscreenWindow(ImVec2(0.0f, heading_height), ImVec2(display_size.x, display_size.y - heading_height),
                              "排行榜", background, 0.0f, 0.0f, 0))
    {
      BeginMenuButtons();

      const auto result = Cheevos::TryEnumerateLeaderboardEntries(
        *s_open_leaderboard_id,
        [rank_column_width, name_column_width, column_spacing](const Cheevos::LeaderboardEntry& lbEntry) -> bool {
          DrawLeaderboardEntry(lbEntry, rank_column_width, name_column_width, column_spacing);
          return true;
        });

      if (!result.has_value())
      {
        ImGui::PushFont(g_large_font);

        const ImVec2 pos_min(0.0f, heading_height);
        const ImVec2 pos_max(display_size.x, display_size.y);
        ImGui::RenderTextClipped(pos_min, pos_max, "正在下载排行榜数据，请稍候...", nullptr, nullptr,
                                 ImVec2(0.5f, 0.5f));

        ImGui::PopFont();
      }

      EndMenuButtons();
    }
    EndFullscreenWindow();
  }

  if (close_leaderboard_on_exit)
    s_open_leaderboard_id.reset();
}

#else

void DrawAchievementWindow() {}
void DrawLeaderboardsWindow() {}

#endif

bool SetControllerNavInput(FrontendCommon::ControllerNavigationButton button, bool value)
{
  s_nav_input_values[static_cast<u32>(button)] = value;
  if (!HasActiveWindow())
    return false;

#if 0
  // This is a bit hacky..
  ImGuiIO& io = ImGui::GetIO();

#define MAP_KEY(nbutton, imkey)                                                                                        \
  if (button == nbutton)                                                                                               \
  {                                                                                                                    \
    io.KeysDown[io.KeyMap[imkey]] = value;                                                                             \
  }

  MAP_KEY(FrontendCommon::ControllerNavigationButton::LeftTrigger, ImGuiKey_PageUp);
  MAP_KEY(FrontendCommon::ControllerNavigationButton::RightTrigger, ImGuiKey_PageDown);

#undef MAP_KEY
#endif

  return true;
}

void SetImGuiNavInputs()
{
  if (!HasActiveWindow())
    return;

  ImGuiIO& io = ImGui::GetIO();

#define MAP_BUTTON(button, imbutton) io.NavInputs[imbutton] = s_nav_input_values[static_cast<u32>(button)] ? 1.0f : 0.0f

  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::Activate, ImGuiNavInput_Activate);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::Cancel, ImGuiNavInput_Cancel);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::DPadLeft, ImGuiNavInput_DpadLeft);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::DPadRight, ImGuiNavInput_DpadRight);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::DPadUp, ImGuiNavInput_DpadUp);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::DPadDown, ImGuiNavInput_DpadDown);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::LeftShoulder, ImGuiNavInput_FocusPrev);
  MAP_BUTTON(FrontendCommon::ControllerNavigationButton::RightShoulder, ImGuiNavInput_FocusNext);

#undef MAP_BUTTON
}

} // namespace FullscreenUI
