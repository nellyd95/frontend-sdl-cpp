#include "ProjectMGUI.h"

#include "AnonymousProFont.h"
#include "LiberationSansFont.h"
#include "ProjectMWrapper.h"
#include "SDLRenderingWindow.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#include <Poco/Path.h>
#include <Poco/NotificationCenter.h>

#include <Poco/Util/Application.h>

#include <algorithm>
#include <cctype>
#include <utility>

const char* ProjectMGUI::name() const
{
    return "Preset Selection GUI";
}

void ProjectMGUI::initialize(Poco::Util::Application& app)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    Poco::Path userConfigurationDir = Poco::Path::configHome();
    userConfigurationDir.makeDirectory().append("projectM/");
    userConfigurationDir.setFileName(app.config().getString("application.baseName") + ".UI.ini");
    _uiIniFileName = userConfigurationDir.toString();

    io.IniFilename = _uiIniFileName.c_str();

    ImGui::StyleColorsDark();

    auto& renderingWindow = Poco::Util::Application::instance().getSubsystem<SDLRenderingWindow>();
    auto& projectMWrapper = Poco::Util::Application::instance().getSubsystem<ProjectMWrapper>();

    _projectMWrapper = &projectMWrapper;
    _renderingWindow = renderingWindow.GetRenderingWindow();
    _glContext = renderingWindow.GetGlContext();

    ImGui_ImplSDL2_InitForOpenGL(_renderingWindow, _glContext);
    ImGui_ImplOpenGL3_Init("#version 150");

    UpdateFontSize();

    // Set a sensible minimum window size to prevent layout assertions
    auto& style = ImGui::GetStyle();
    style.WindowMinSize = {128, 128};

    Poco::NotificationCenter::defaultCenter().addObserver(_displayToastNotificationObserver);
}

void ProjectMGUI::uninitialize()
{
    Poco::NotificationCenter::defaultCenter().removeObserver(_displayToastNotificationObserver);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    _projectMWrapper = nullptr;
    _renderingWindow = nullptr;
    _glContext = nullptr;
}

void ProjectMGUI::UpdateFontSize()
{
    ImGuiIO& io = ImGui::GetIO();

    auto displayIndex = SDL_GetWindowDisplayIndex(_renderingWindow);
    if (displayIndex < 0)
    {
        poco_debug_f1(_logger, "Could not get display index for application window: %s", std::string(SDL_GetError()));
        return;
    }

    auto newScalingFactor = GetScalingFactor();

    // Only interested in changes of .05 or more
    if (std::abs(_textScalingFactor - newScalingFactor) < 0.05)
    {
        return;
    }

    poco_debug_f3(_logger, "Scaling factor change for display %?d: %hf -> %hf", displayIndex, _textScalingFactor, newScalingFactor);

    _textScalingFactor = newScalingFactor;

    ImFontConfig config;
    config.MergeMode = true;

    io.Fonts->Clear();
    _uiFont = io.Fonts->AddFontFromMemoryCompressedTTF(&AnonymousPro_compressed_data, AnonymousPro_compressed_size, floor(24.0f * _textScalingFactor));
    _toastFont = io.Fonts->AddFontFromMemoryCompressedTTF(&LiberationSans_compressed_data, LiberationSans_compressed_size, floor(40.0f * _textScalingFactor));
    io.Fonts->Build();

    ImGui::GetStyle().ScaleAllSizes(1.0);
}

void ProjectMGUI::ProcessInput(const SDL_Event& event)
{
    ImGui_ImplSDL2_ProcessEvent(&event);
}

void ProjectMGUI::Toggle()
{
    _visible = !_visible;
}

void ProjectMGUI::Visible(bool visible)
{
    _visible = visible;
}

bool ProjectMGUI::Visible() const
{
    return _visible;
}

void ProjectMGUI::Draw()
{
    // Don't render UI at all if there's no need.
    if (!_toast && !_visible && !_presetSearchOpen)
    {
        return;
    }

    if (_userScalingFactor != GetClampedUserScalingFactor())
    {
        UpdateFontSize();
    }

    ImGui_ImplSDL2_NewFrame();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    float secondsSinceLastFrame = .0f;
    if (_lastFrameTicks == 0)
    {
        _lastFrameTicks = SDL_GetTicks64();
    }
    else
    {
        auto currentFrameTicks = SDL_GetTicks64();
        secondsSinceLastFrame = static_cast<float>(currentFrameTicks - _lastFrameTicks) * .001f;
        _lastFrameTicks = currentFrameTicks;
    }

    if (_toast)
    {
        if (!_toast->Draw(secondsSinceLastFrame))
        {
            _toast.reset();
        }
    }

    if (_visible)
    {
        _mainMenu.Draw();
        _settingsWindow.Draw();
        _aboutWindow.Draw();
        _helpWindow.Draw();
    }

    if (_presetSearchOpen)
    {
        DrawPresetSearchPopup();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool ProjectMGUI::WantsKeyboardInput()
{
    auto& io = ImGui::GetIO();
    return io.WantCaptureKeyboard;
}

bool ProjectMGUI::WantsMouseInput()
{
    auto& io = ImGui::GetIO();
    return io.WantCaptureMouse;
}

void ProjectMGUI::PushToastFont()
{
    ImGui::PushFont(_toastFont);
}

void ProjectMGUI::PushUIFont()
{
    ImGui::PushFont(_uiFont);
}

void ProjectMGUI::PopFont()
{
    ImGui::PopFont();
}

void ProjectMGUI::ShowSettingsWindow()
{
    _settingsWindow.Show();
}

void ProjectMGUI::ShowAboutWindow()
{
    _aboutWindow.Show();
}

void ProjectMGUI::ShowHelpWindow()
{
    _helpWindow.Show();
}

void ProjectMGUI::OpenPresetSearch()
{
    _presetSearchOpen = true;
    _presetSearchQuery[0] = '\0';
    _presetSearchSelection = 0;
    RefreshPresetSearchMatches();
}

void ProjectMGUI::DrawPresetSearchPopup()
{
    ImGui::SetNextWindowSize(ImVec2(800, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    bool open = _presetSearchOpen;
    if (!ImGui::Begin("Preset Search###PresetSearch", &open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        _presetSearchOpen = open;
        return;
    }

    ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_AutoSelectAll;
    if (ImGui::InputText("Find preset", _presetSearchQuery, IM_ARRAYSIZE(_presetSearchQuery), inputFlags))
    {
        _presetSearchSelection = 0;
        RefreshPresetSearchMatches();
    }

    const bool windowAppearing = ImGui::IsWindowAppearing();
    if (windowAppearing)
    {
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::Separator();
    ImGui::Text("Matches: %d", static_cast<int>(_presetSearchMatches.size()));
    ImGui::BeginChild("PresetSearchResults", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()));

    for (int i = 0; i < static_cast<int>(_presetSearchMatches.size()); ++i)
    {
        auto playlistIndex = _presetSearchMatches[i];
        auto presetName = projectm_playlist_item(_projectMWrapper->Playlist(), playlistIndex);
        std::string displayText = presetName ? Poco::Path(presetName).getFileName() : "<unknown>";
        if (presetName)
        {
            projectm_playlist_free_string(presetName);
        }

        if (ImGui::Selectable(displayText.c_str(), i == _presetSearchSelection))
        {
            _presetSearchSelection = i;
        }
    }

    ImGui::EndChild();

    bool activateSelection = false;
    const bool enterPressed = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    if (ImGui::Button("Load Selected") || (!windowAppearing && enterPressed))
    {
        activateSelection = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        open = false;
    }

    if (!_presetSearchMatches.empty())
    {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
        {
            _presetSearchSelection = std::min(_presetSearchSelection + 1, static_cast<int>(_presetSearchMatches.size()) - 1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
        {
            _presetSearchSelection = std::max(_presetSearchSelection - 1, 0);
        }
    }

    if (activateSelection && !_presetSearchMatches.empty())
    {
        auto playlistIndex = _presetSearchMatches[_presetSearchSelection];
        projectm_playlist_set_position(_projectMWrapper->Playlist(), playlistIndex, true);
        open = false;
    }

    ImGui::End();
    _presetSearchOpen = open;
}

void ProjectMGUI::RefreshPresetSearchMatches()
{
    _presetSearchMatches.clear();

    auto playlist = _projectMWrapper->Playlist();
    auto playlistSize = projectm_playlist_size(playlist);
    std::string query = _presetSearchQuery;
    std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (uint32_t i = 0; i < playlistSize; ++i)
    {
        auto presetName = projectm_playlist_item(playlist, i);
        if (!presetName)
        {
            continue;
        }

        std::string fullName = presetName;
        projectm_playlist_free_string(presetName);

        if (query.empty())
        {
            _presetSearchMatches.push_back(i);
            continue;
        }

        std::string lowerName = fullName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerName.find(query) != std::string::npos)
        {
            _presetSearchMatches.push_back(i);
        }
    }
}

float ProjectMGUI::GetScalingFactor()
{
    int windowWidth;
    int windowHeight;
    int renderWidth;
    int renderHeight;

    SDL_GetWindowSize(_renderingWindow, &windowWidth, &windowHeight);
    SDL_GL_GetDrawableSize(_renderingWindow, &renderWidth, &renderHeight);

    _userScalingFactor = GetClampedUserScalingFactor();

    // If the OS has a scaled UI, this will return the inverse factor. E.g. if the display is scaled to 200%,
    // the renderWidth (in actual pixels) will be twice as much as the "virtual" unscaled window width.
    return ((static_cast<float>(windowWidth) / static_cast<float>(renderWidth)) + (static_cast<float>(windowHeight) / static_cast<float>(renderHeight))) * 0.5f * _userScalingFactor;
}

float ProjectMGUI::GetClampedUserScalingFactor()
{
    return std::min(3.0f, std::max(0.1f, static_cast<float>(Poco::Util::Application::instance().config().getDouble("window.uiScale", 1.0))));
}

void ProjectMGUI::DisplayToastNotificationHandler(const Poco::AutoPtr<DisplayToastNotification>& notification)
{
    if (Poco::Util::Application::instance().config().getBool("projectM.displayToasts", true))
    {
        _toast = std::make_unique<ToastMessage>(notification->ToastText(), 3.0f);
    }
}
