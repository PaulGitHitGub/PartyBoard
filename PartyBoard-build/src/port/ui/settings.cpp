// Credits: TwilitRealm

#include "settings.hpp"

#include "aurora/gfx.h"
#include "bool_button.hpp"
#include "controller_config.hpp"
#include "player_setup.hpp"
#include "port/app_update.hpp"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if (defined(__APPLE__) && !TARGET_OS_IPHONE) || defined(_WIN32)
#include "port/send_logs.h"
#endif
#include "port/config.hpp"
#include "port/display_rate.hpp"
#include "port/window_mode.hpp"
#include "port/netplay_runtime.h"
#include "localization.hpp"
#include "../imgui/ImGuiEngine.hpp"
#include "../file_select.hpp"
#include "graphics_tuner.hpp"
#include "menu_bar.hpp"
#include "number_button.hpp"
#include "pane.hpp"
#include "phone_controller.hpp"
#include "string_button.hpp"
#include "prelaunch.hpp"
#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <aurora/aurora.h>
#include <game/disp.h>
#include <gx/GXAurora.h>
#include <vi.h>

extern "C" int PartyBoard_TargetFrameRateFor(bool netplayEnabled, int configured);

namespace partyboard::ui {
namespace {

    struct LanguageChoice {
        const char *name;
        GameLanguage value;
    };

    // Every language a European disc carries (mess/*_g|f|s|i.dat); a USA disc is English only.
    constexpr std::array kLanguageChoices = {
        LanguageChoice { "English", GameLanguage::English },
        LanguageChoice { "French", GameLanguage::French },
        LanguageChoice { "German", GameLanguage::German },
        LanguageChoice { "Spanish", GameLanguage::Spanish },
        LanguageChoice { "Italian", GameLanguage::Italian },
    };

    constexpr std::array kCardFileTypes = {
        "Card Image",
        "GCI Folder",
    };

    constexpr std::array kFpsOverlayCornerNames = {
        "Top Left",
        "Top Right",
        "Bottom Left",
        "Bottom Right",
    };

    constexpr std::array kTargetFrameRates = {
        60,
        90,
        120,
        144,
        165,
        240,
    };

    constexpr std::array kGyroInputModeLabels = {
        "Sensor",
        "Mouse",
    };

    bool try_parse_backend(std::string_view backend, AuroraBackend &outBackend)
    {
        if (backend == "auto") {
            outBackend = BACKEND_AUTO;
            return true;
        }
        if (backend == "d3d11") {
            outBackend = BACKEND_D3D11;
            return true;
        }
        if (backend == "d3d12") {
            outBackend = BACKEND_D3D12;
            return true;
        }
        if (backend == "metal") {
            outBackend = BACKEND_METAL;
            return true;
        }
        if (backend == "vulkan") {
            outBackend = BACKEND_VULKAN;
            return true;
        }
        if (backend == "opengl") {
            outBackend = BACKEND_OPENGL;
            return true;
        }
        if (backend == "opengles") {
            outBackend = BACKEND_OPENGLES;
            return true;
        }
        if (backend == "webgpu") {
            outBackend = BACKEND_WEBGPU;
            return true;
        }
        if (backend == "null") {
            outBackend = BACKEND_NULL;
            return true;
        }

        return false;
    }

    std::string_view backend_name(AuroraBackend backend)
    {
        switch (backend) {
            default:
                return "Auto";
            case BACKEND_D3D12:
                return "D3D12";
            case BACKEND_D3D11:
                return "D3D11";
            case BACKEND_METAL:
                return "Metal";
            case BACKEND_VULKAN:
                return "Vulkan";
            case BACKEND_OPENGL:
                return "OpenGL";
            case BACKEND_OPENGLES:
                return "OpenGL ES";
            case BACKEND_WEBGPU:
                return "WebGPU";
            case BACKEND_NULL:
                return "Null";
        }
    }

    std::string_view backend_id(AuroraBackend backend)
    {
        switch (backend) {
            default:
                return "auto";
            case BACKEND_D3D12:
                return "d3d12";
            case BACKEND_D3D11:
                return "d3d11";
            case BACKEND_METAL:
                return "metal";
            case BACKEND_VULKAN:
                return "vulkan";
            case BACKEND_OPENGL:
                return "opengl";
            case BACKEND_OPENGLES:
                return "opengles";
            case BACKEND_WEBGPU:
                return "webgpu";
            case BACKEND_NULL:
                return "null";
        }
    }

    std::vector<AuroraBackend> available_backends()
    {
        std::vector<AuroraBackend> backends;
        backends.emplace_back(BACKEND_AUTO);
        size_t backendCount = 0;
        const AuroraBackend *raw = aurora_get_available_backends(&backendCount);
        for (size_t i = 0; i < backendCount; ++i) {
            // Do not expose NULL or D3D11
            if (raw[i] != BACKEND_NULL && raw[i] != BACKEND_D3D11) {
                backends.emplace_back(raw[i]);
            }
        }
        return backends;
    }

    AuroraBackend configured_backend()
    {
        AuroraBackend configuredBackend = BACKEND_AUTO;
        const auto configuredId = getSettings().backend.graphicsBackend.getValue();
        if (!try_parse_backend(configuredId, configuredBackend)) {
            configuredBackend = BACKEND_AUTO;
        }
        return configuredBackend;
    }

    // TODO PC
    void reset_for_speedrun_mode()
    {
        getSettings().game.infiniteHearts.setValue(false);

        getSettings().game.enableTurboKeybind.setValue(false);
    }

    constexpr std::array<const char *, 3> kTouchControlModes = { "Automatic", "Always", "Never" };

    const Rml::String kInternalResolutionHelpText = "How sharp the 3D scene is drawn. Higher needs a faster graphics card.";
    const Rml::String kShadowResolutionHelpText = "Sharper shadows. Higher needs more graphics memory.";
    const Rml::String kBloomHelpText = "Configure the post-processing bloom effect. Classic uses the original bloom pass; Dusk uses "
                                       "a higher-quality bloom pass.";
    const Rml::String kBloomBrightnessHelpText = "Configure bloom intensity. Higher values make bright areas glow more strongly.";
    const Rml::String kUnlockFramerateHelpText = "Uses inter-frame interpolation to enable higher frame rates.<br/><br/>May introduce minor "
                                                 "visual artifacts or animation glitches.";

    int float_setting_percent(ConfigVar<float> &var)
    {
        return static_cast<int>(var.getValue() * 100.0f + 0.5f);
    }

    struct ConfigBoolProps {
        Rml::String key;
        Rml::String icon;
        Rml::String helpText;
        std::function<void(bool)> onChange;
        std::function<bool()> isDisabled;
    };

    SelectButton &config_bool_select(Pane &leftPane, Pane &rightPane, ConfigVar<bool> &var, ConfigBoolProps props)
    {
        auto &button = leftPane.add_child<BoolButton>(BoolButton::Props {
            .key = std::move(props.key),
            .icon = std::move(props.icon),
            .getValue = [&var] { return var.getValue(); },
            .setValue =
                [&var, callback = std::move(props.onChange)](bool value) {
                    if (value == var.getValue()) {
                        return;
                    }
                    var.setValue(value);
                    config::Save();
                    if (callback) {
                        callback(value);
                    }
                },
            .isDisabled = std::move(props.isDisabled),
            .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
        });
        leftPane.register_control(button, rightPane, [helpText = std::move(props.helpText)](Pane &pane) {
            pane.clear();
            pane.add_rml(helpText);
        });
        return button;
    }

    SelectButton &config_percent_select(Pane &leftPane, Pane &rightPane, ConfigVar<float> &var, Rml::String key, Rml::String helpText, int min,
        int max, int step = 5, std::function<bool()> isDisabled = {})
    {
        auto &button = leftPane.add_child<NumberButton>(NumberButton::Props {
            .key = std::move(key),
            .getValue = [&var] { return float_setting_percent(var); },
            .setValue =
                [&var, min, max](int value) {
                    var.setValue(std::clamp(value, min, max) / 100.0f);
                    config::Save();
                },
            .isDisabled = std::move(isDisabled),
            .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
            .min = min,
            .max = max,
            .step = step,
            .suffix = "%",
        });
        leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane &pane) {
            pane.clear();
            pane.add_text(helpText);
        });
        return button;
    }

    template <typename T>
    void graphics_tuner_control(Window &window, Pane &leftPane, Pane &rightPane, ConfigVar<T> &var, const GraphicsTunerProps &props, bool prelaunch)
    {
        leftPane.register_control(leftPane
                                      .add_select_button({
                                          .key = props.title,
                                          .getValue =
                                              [&var, option = props.option] {
                                                  if constexpr (std::is_same_v<T, float>) {
                                                      return format_graphics_setting_value(option, float_setting_percent(var));
                                                  }
                                                  else {
                                                      return format_graphics_setting_value(option, static_cast<int>(var.getValue()));
                                                  }
                                              },
                                          .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
                                          .submit = false,
                                      })
                                      .on_nav_command([&window, props, prelaunch](Rml::Event &, NavCommand cmd) {
                                          if (cmd == NavCommand::Confirm || cmd == NavCommand::Left || cmd == NavCommand::Right) {
                                              window.push(std::make_unique<GraphicsTuner>(props, prelaunch));
                                              return true;
                                          }
                                          return false;
                                      }),
            rightPane, [helpText = props.helpText](Pane &pane) {
                pane.clear();
                pane.add_text(helpText);
            });
    }

} // namespace

SettingsWindow::SettingsWindow(bool prelaunch)
    : mPrelaunch(prelaunch)
{
    if (prelaunch) {
        mSuppressNavFallback = true;
        add_tab("Prelaunch", [this](Rml::Element *content) {
            auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
            auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

            leftPane.register_control(leftPane
                                          .add_select_button({
                                              .key = "Disc Image",
                                              .getValue =
                                                  [] {
                                                      const auto &path = prelaunch_state().configuredDiscPath;
                                                      std::string display;
                                                      if (path.empty()) {
                                                          display = "(none)";
                                                      }
                                                      else {
                                                          display = display_name_for_path(path);
                                                          if (display.empty()) {
                                                              display = path;
                                                          }
                                                      }
                                                      return display;
                                                  },
                                              .isModified =
                                                  [] {
                                                      const auto &state = prelaunch_state();
                                                      const auto &active = state.activeDiscPath;
                                                      return !active.empty() && state.configuredDiscPath != active;
                                                  },
                                          })
                                          .on_pressed([] { open_iso_picker(); }),
                rightPane, [](Pane &pane) {
                    pane.add_rml("The Mario Party 4 disc image to play. Takes effect after a restart.");
                });
            leftPane.register_control(leftPane.add_select_button({
                                          .key = "Language",
                                          .getValue =
                                              [] {
                                                  for (const auto &choice : kLanguageChoices) {
                                                      if (getSettings().game.language.getValue() == choice.value) {
                                                          return choice.name;
                                                      }
                                                  }
                                                  return kLanguageChoices[0].name;
                                              },
                                          .isDisabled =
                                              [] {
                                                  const auto &state = prelaunch_state();
                                                  return !state.configuredDiscCanLaunch || !state.configuredDiscInfo.isPal;
                                              },
                                          .isModified = [] { return getSettings().game.language.getValue() != prelaunch_state().initialLanguage; },
                                      }),
                rightPane, [](Pane &pane) {
                    for (const auto &choice : kLanguageChoices) {
                        pane.add_button({
                                            .text = choice.name,
                                            .isSelected = [value = choice.value] { return getSettings().game.language.getValue() == value; },
                                        })
                            .on_pressed([value = choice.value] {
                                // mDoAud_seStartMenu(kSoundItemChange); // TODO PC
                                getSettings().game.language.setValue(value);
                                config::Save();
                            });
                    }
                    pane.add_rml("Takes effect after a restart.");
                });
            leftPane.register_control(
                leftPane.add_select_button({
                    .key = "Graphics Backend",
                    .getValue = [] { return Rml::String { backend_name(configured_backend()) }; },
                    .isModified = [] { return getSettings().backend.graphicsBackend.getValue() != prelaunch_state().initialGraphicsBackend; },
                }),
                rightPane, [](Pane &pane) {
                    const auto availableBackends = available_backends();
                    for (const auto backend : availableBackends) {
                        pane.add_button({
                                            .text = Rml::String { backend_name(backend) },
                                            .isSelected = [backend] { return configured_backend() == backend; },
                                        })
                            .on_pressed([backend] {
                                // mDoAud_seStartMenu(kSoundItemChange); // TODO PC
                                getSettings().backend.graphicsBackend.setValue(std::string { backend_id(backend) });
                                config::Save();
                            });
                    }
                    pane.add_rml("Takes effect after a restart.");
                });
            leftPane.register_control(
                leftPane.add_select_button({
                    .key = "Save File Type",
                    .getValue = [] { return kCardFileTypes[getSettings().backend.cardFileType.getValue()]; },
                    .isModified = [] { return getSettings().backend.cardFileType.getValue() != prelaunch_state().initialCardFileType; },
                }),
                rightPane, [](Pane &pane) {
                    for (int i = 0; i < kCardFileTypes.size(); i++) {
                        pane.add_button({
                                            .text = kCardFileTypes[i],
                                            .isSelected = [i] { return getSettings().backend.cardFileType.getValue() == i; },
                                        })
                            .on_pressed([i] {
                                // mDoAud_seStartMenu(kSoundItemChange); // TODO PC
                                getSettings().backend.cardFileType.setValue(i);
                                config::Save();
                            });
                    }
                });
        });
    }

    add_tab("Video", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Display");

        // There is no window on a phone: SDL answers "leave fullscreen" by
        // bringing back the status and navigation bars over the game.
        if constexpr (!IsMobile) {
            auto &windowModeRow = leftPane.add_select_button({
                                          .key = "Window Mode",
                                          .getValue = [] {
                                              return Rml::String { getSettings().video.enableFullscreen.getValue()
                                                      ? "Fullscreen" : "Windowed" };
                                          },
                                          .isModified = [] {
                                              return getSettings().video.enableFullscreen.getValue()
                                                  != getSettings().video.enableFullscreen.getDefaultValue();
                                          },
                                      });
#if defined(PARTYBOARD_HD_BETA)
            mTestVideoRow = &windowModeRow;
#endif
            leftPane.register_control(windowModeRow, rightPane, [](Pane &pane) {
                    pane.add_button({
                                        .text = "Windowed",
                                        .isSelected = [] { return !getSettings().video.enableFullscreen.getValue(); },
                                    })
                        .on_pressed([] { window_mode::set_fullscreen(false, "menu"); });
                    pane.add_button({
                                        .text = "Fullscreen",
                                        .isSelected = [] { return getSettings().video.enableFullscreen.getValue(); },
                                    })
                        .on_pressed([] { window_mode::set_fullscreen(true, "menu"); });
#if defined(__APPLE__)
                    pane.add_rml("Fullscreen covers the whole screen, with no menu bar. Shortcut: Cmd+Ctrl+F.");
#else
                    pane.add_rml("Fullscreen covers the whole screen, taskbar included. Shortcut: Alt+Enter.");
#endif
                });
            leftPane.register_control(leftPane.add_button("Restore Default Window Size").on_pressed([] {
                // mDoAud_seStartMenu(kSoundItemChange); // TODO PC
                window_mode::set_fullscreen(false, "Restore Default Window Size");
                VISetWindowSize(HU_FB_WIDTH * 2, HU_FB_HEIGHT * 2);
                VICenterWindow();
            }),
                rightPane, [](Pane &pane) { pane.clear(); });
        }
        config_bool_select(leftPane, rightPane, getSettings().video.enableVsync,
            {
                .key = "Enable VSync",
                .helpText = "Match the screen's refresh to avoid tearing.",
                .onChange = [](bool value) { aurora_enable_vsync(value); },
            });
        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Frame Rate",
                                      .getValue = [] {
                                          return Rml::String { std::to_string(getSettings().video.targetFrameRate.getValue()) + " FPS" };
                                      },
                                      .isModified = [] {
                                          return getSettings().video.targetFrameRate.getValue()
                                              != getSettings().video.targetFrameRate.getDefaultValue();
                                      },
                                  }),
            rightPane, [](Pane &pane) {
                // On a phone, only rates the screen shows evenly: a 60/120 Hz screen offers 60 and
                // 120, a 144 Hz one 144 (120 there would judder between two refresh intervals).
                const auto screenRates = display::supported_refresh_rates();
                const int screenMax = screenRates.empty() ? 0 : screenRates.back();
                const auto shownEvenly = [&screenRates](int frameRate) {
                    return std::ranges::any_of(screenRates, [frameRate](int refresh) {
                        const float ratio = static_cast<float>(refresh) / static_cast<float>(frameRate);
                        return ratio >= 0.99f && std::abs(ratio - std::round(ratio)) < 0.02f;
                    });
                };
                for (const int frameRate : kTargetFrameRates) {
                    if (!screenRates.empty() && frameRate > kTargetFrameRates[0] && !shownEvenly(frameRate)) {
                        continue;
                    }
                    pane.add_button({
                                        .text = Rml::String { std::to_string(frameRate) + " FPS" },
                                        .isSelected = [frameRate] {
                                            return getSettings().video.targetFrameRate.getValue() == frameRate;
                                        },
                                    })
                        .on_pressed([frameRate] {
                            getSettings().video.targetFrameRate.setValue(frameRate);
                            config::Save();
                            display::request_frame_rate(PartyBoard_TargetFrameRateFor(PartyBoard_NetplayEnabled(), frameRate));
                        });
                }
                if (screenMax > 0) {
                    pane.add_rml(fmt::format(fmt::runtime(ui_translate("This screen refreshes at up to {} Hz.")), screenMax));
                }
                pane.add_rml("Smoother motion only: the game itself still runs at 60 Hz, with no extra input delay.");
            });
        config_bool_select(leftPane, rightPane, getSettings().video.lockAspectRatio,
            {
                .key = "Lock 4:3 Aspect Ratio",
                .helpText = "Keep the original 4:3 picture.",
                .onChange = [](bool value) {
                    if (value) {
                        getSettings().video.enableAdaptiveWidescreen.setValue(false);
                    }
                    AuroraSetViewportPolicy(value ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH);
                    config::Save();
                },
            });
        config_bool_select(leftPane, rightPane, getSettings().video.enableAdaptiveWidescreen,
            {
                .key = "Adaptive Widescreen HUD",
                .helpText = "Fill a wide window without stretching; the HUD moves to the edges.",
                .onChange = [](bool value) {
                    if (value) {
                        getSettings().video.lockAspectRatio.setValue(false);
                    }
                    AuroraSetViewportPolicy(value || !getSettings().video.lockAspectRatio.getValue()
                            ? AURORA_VIEWPORT_STRETCH
                            : AURORA_VIEWPORT_FIT);
                    config::Save();
                },
            });
        config_bool_select(leftPane, rightPane, getSettings().game.pauseOnFocusLost,
            {
                .key = "Pause on Focus Lost",
                .isDisabled = [] { return IsMobile; },
            });
        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Show FPS Counter",
                                      .getValue =
                                          [] {
                                              if (!getSettings().video.enableFpsOverlay.getValue()) {
                                                  return Rml::String { "Off" };
                                              }
                                              const int idx = getSettings().video.fpsOverlayCorner.getValue();
                                              return Rml::String { kFpsOverlayCornerNames[idx] };
                                          },
                                      .isModified =
                                          [] {
                                              const auto &enable = getSettings().video.enableFpsOverlay;
                                              const auto &corner = getSettings().video.fpsOverlayCorner;
                                              return enable.getValue() != enable.getDefaultValue()
                                                  || (enable.getValue() && corner.getValue() != corner.getDefaultValue());
                                          },
                                  }),
            rightPane, [](Pane &pane) {
                pane.add_button({
                                    .text = "Off",
                                    .isSelected = [] { return !getSettings().video.enableFpsOverlay.getValue(); },
                                })
                    .on_pressed([] {
                        // mDoAud_seStartMenu(kSoundItemChange); // TODO PC
                        getSettings().video.enableFpsOverlay.setValue(false);
                        config::Save();
                    });
                for (int i = 0; i < static_cast<int>(kFpsOverlayCornerNames.size()); ++i) {
                    pane
                        .add_button({
                            .text = kFpsOverlayCornerNames[i],
                            .isSelected
                            = [i] { return getSettings().video.enableFpsOverlay.getValue() && getSettings().video.fpsOverlayCorner.getValue() == i; },
                        })
                        .on_pressed([i] {
                            // mDoAud_seStartMenu(kSoundItemChange); // TODO PC
                            getSettings().video.enableFpsOverlay.setValue(true);
                            getSettings().video.fpsOverlayCorner.setValue(i);
                            config::Save();
                        });
                }
                pane.add_rml("Shows the frame rate in a corner while you play.");
            });

        leftPane.add_section("Resolution");
        graphics_tuner_control(*this, leftPane, rightPane, getSettings().game.internalResolutionScale,
            GraphicsTunerProps {
                .option = GraphicsOption::InternalResolution,
                .title = "Internal Resolution",
                .helpText = kInternalResolutionHelpText,
                .valueMin = 0,
                .valueMax = 12,
                .defaultValue = 1,
            },
            mPrelaunch);
        graphics_tuner_control(*this, leftPane, rightPane, getSettings().game.shadowResolutionMultiplier,
            GraphicsTunerProps {
                .option = GraphicsOption::ShadowResolution,
                .title = "Shadow Resolution",
                .helpText = kShadowResolutionHelpText,
                .valueMin = 1,
                .valueMax = 8,
                .defaultValue = 1,
            },
            mPrelaunch);

        // leftPane.add_section("Post-Processing");

        // leftPane.add_section("Rendering");
    });

    add_tab("Input", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        auto addOption = [&](const Rml::String &key, ConfigVar<bool> &value, const Rml::String &helpText, std::function<bool()> isDisabled = {}) {
            config_bool_select(leftPane, rightPane, value,
                {
                    .key = key,
                    .helpText = helpText,
                    .isDisabled = std::move(isDisabled),
                });
        };

        leftPane.add_section("Controller");
        leftPane.register_control(
            leftPane.add_button("Set Up Players").on_pressed([this] { push(std::make_unique<PlayerSetupWindow>()); }), rightPane,
            [](Pane &pane) {
                pane.clear();
                pane.add_text("Everyone holds L + R on their controller to become player 1, 2, 3 and 4 in turn.");
            });
        leftPane.register_control(
            leftPane.add_button("Configure Controller").on_pressed([this] { push(std::make_unique<ControllerConfigWindow>()); }), rightPane,
            [](Pane &pane) {
                pane.clear();
                pane.add_text("Open controller binding configuration.");
            });
        config_bool_select(leftPane, rightPane, getSettings().game.allowBackgroundInput,
            {
                .key = "Allow Background Input",
                .helpText = "Controllers keep working when the window is in the background.",
                .onChange = [](bool value) { aurora_set_background_input(value); },
            });

        leftPane.add_section("Screen Controller");
        leftPane.register_control(leftPane.add_select_button({
                                      .key = "Screen Controller",
                                      .getValue = [] { return Rml::String { kTouchControlModes[std::clamp(getSettings().game.touchControls.getValue(), 0, 2)] }; },
                                      .isModified =
                                          [] {
                                              const auto &v = getSettings().game.touchControls;
                                              return v.getValue() != v.getDefaultValue();
                                          },
                                  }),
            rightPane, [](Pane &pane) {
                for (int i = 0; i < static_cast<int>(kTouchControlModes.size()); ++i) {
                    pane.add_button({
                                        .text = kTouchControlModes[i],
                                        .isSelected = [i] { return getSettings().game.touchControls.getValue() == i; },
                                    })
                        .on_pressed([i] {
                            getSettings().game.touchControls.setValue(i);
                            config::Save();
                        });
                }
                pane.add_text("On-screen buttons for phones and tablets. Automatic shows them when no gamepad is on port 1.");
            });
        leftPane.register_control(leftPane.add_child<NumberButton>(NumberButton::Props {
                                      .key = "Screen Controller Opacity",
                                      .getValue = [] { return getSettings().game.touchControlsOpacity.getValue(); },
                                      .setValue =
                                          [](int value) {
                                              getSettings().game.touchControlsOpacity.setValue(std::clamp(value, 10, 100));
                                              config::Save();
                                          },
                                      .isModified =
                                          [] {
                                              const auto &v = getSettings().game.touchControlsOpacity;
                                              return v.getValue() != v.getDefaultValue();
                                          },
                                      .min = 10,
                                      .max = 100,
                                      .step = 10,
                                      .suffix = "%",
                                  }),
            rightPane, [](Pane &pane) { pane.add_text("How visible the screen controller is over the game."); });

        leftPane.add_section("Phone Controller");
        auto &phoneRow = leftPane.add_select_button({
            .key = "Connect Phone",
            .getValue = [] { return phone_controller_status_label(); },
        });
        leftPane.register_control(phoneRow, rightPane, [](Pane &pane) { pane.add_child<PhoneControllerPanel>(); });
#if defined(PARTYBOARD_HD_BETA)
        mTestPhoneRow = &phoneRow;
#endif

        leftPane.add_section("Tools");
        addOption("Turbo Key", getSettings().game.enableTurboKeybind, "Hold Tab to unlock the FPS, speeding up the game.",
            [] { return getSettings().game.speedrunMode; });
    });

    // add_tab("Audio", [this](Rml::Element *content) {
    //     auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    //     auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);
    //
    //     // TODO: Individual sliders for Main Music, Sub Music, Sound Effects, and Fanfare.
    //     // leftPane.add_section("Volume");
    //     // leftPane.register_control(
    //     //     leftPane.add_child<NumberButton>(NumberButton::Props {
    //     //         .key = "Master Volume",
    //     //         .getValue = [] { return getSettings().audio.masterVolume.getValue(); },
    //     //         .setValue =
    //     //             [](int value) {
    //     //                 getSettings().audio.masterVolume.setValue(value);
    //     //                 config::Save();
    //     //                 // audio::SetMasterVolume(value / 100.f); // TODO PC
    //     //             },
    //     //         .isModified = [] { return getSettings().audio.masterVolume.getValue() != getSettings().audio.masterVolume.getDefaultValue(); },
    //     //         .max = 100,
    //     //         .suffix = "%",
    //     //     }),
    //     //     rightPane, [](Pane &pane) {
    //     //         pane.clear();
    //     //         pane.add_text("Adjusts the volume of all sounds in the game.");
    //     //     });
    //
    //     leftPane.add_section("Effects");
    //     // config_bool_select(leftPane, rightPane, getSettings().audio.enableReverb,
    //     //     {
    //     //         .key = "Enable Reverb",
    //     //         .helpText = "Enables the reverb effect in game audio.",
    //     //         .onChange = [](bool value) {
    //     //             // audio::SetEnableReverb(value); // TODO PC
    //     //         },
    //     //     });
    //     // config_bool_select(leftPane, rightPane, getSettings().audio.enableHrtf,
    //     //     {
    //     //         .key = "Enable Spatial Sound",
    //     //         .helpText = "Emulate surround sound via HRTF. Recommended only for use with headphones!",
    //     //         .onChange = [](bool value) {
    //     //             // audio::EnableHrtf = value; // TODO PC
    //     //         },
    //     //     });
    //     // config_bool_select(leftPane, rightPane, getSettings().audio.menuSounds,
    //     //     {
    //     //         .key = "Party Board Menu Sounds",
    //     //         .helpText = "Play sound effects when navigating the Party Board menu.",
    //     //     });
    //
    //     leftPane.add_section("Tweaks");
    // });

    add_tab("Gameplay", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        auto addOption = [&](const Rml::String &key, ConfigVar<bool> &value, const Rml::String &helpText) {
            config_bool_select(leftPane, rightPane, value,
                {
                    .key = key,
                    .helpText = helpText,
                });
        };
        auto addSpeedrunDisabledOption = [&](const Rml::String &key, ConfigVar<bool> &value, const Rml::String &helpText) {
            config_bool_select(leftPane, rightPane, value,
                {
                    .key = key,
                    .helpText = helpText,
                    .isDisabled = [] { return getSettings().game.speedrunMode; },
                });
        };

        // leftPane.add_section("General");
        // leftPane.add_section("Difficulty");
        // leftPane.add_section("Quality of Life");
        leftPane.add_section("Speedrunning");
        config_bool_select(leftPane, rightPane, getSettings().game.speedrunMode,
            {
                .key = "Speedrun Mode",
                .helpText = "Speedrun options; some gameplay changes are turned off.",
                .onChange = [](bool) { reset_for_speedrun_mode(); },
            });
    });

    add_tab("Cheats", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        auto addCheat = [&](const Rml::String &key, ConfigVar<bool> &value, const Rml::String &helpText) {
            config_bool_select(leftPane, rightPane, value,
                {
                    .key = key,
                    .helpText = helpText,
                    .isDisabled = [] { return getSettings().game.speedrunMode; },
                });
        };

        leftPane.add_section("Minigames");
        addCheat("Unlock All Minigames", getSettings().game.unlockAllMinigames,
            "Treat every minigame as unlocked when the game checks minigame availability.");

        leftPane.add_section("Boards");
        addCheat("Unlock Bowser's Gnarly Party", getSettings().game.unlockBowsersGnarlyParty,
            "Treat Bowser's Gnarly Party as unlocked when the game checks board availability.");

        // leftPane.add_section("Resources");
        // addCheat("Infinite Hearts", getSettings().game.infiniteHearts, "Keeps your health full.");

        // leftPane.add_section("Abilities");
    });

    add_tab("Interface", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Party Board");
        if (update::supported()) {
            config_bool_select(leftPane, rightPane, getSettings().backend.checkForUpdates,
                {
                    .key = "Check for Updates",
#ifdef __ANDROID__
                    .helpText = "Check GitHub for a newer build at startup. Nothing downloads without asking.",
#else
                    .helpText = "Look for a newer PartyBoard at startup. Nothing is downloaded.",
#endif
                });
            leftPane.register_control(leftPane.add_button("Check Now").on_pressed([] { update::check(false); }), rightPane,
                [](Pane &pane) { pane.add_text("The answer shows on the home screen, under the version."); });
        }
#if PARTY_BOARD_CAN_OPEN_DATA_FOLDER
        leftPane.register_control(leftPane.add_button("Open Data Folder").on_pressed([] {
            // mDoAud_seStartMenu(kSoundClick); // TODO PC
            partyboard::OpenDataFolder();
        }),
            rightPane, [](Pane &pane) {
                pane.add_text("Settings, saves, logs and texture packs.");
            });
#endif
#if (defined(__APPLE__) && !TARGET_OS_IPHONE) || defined(_WIN32)
        leftPane.register_control(leftPane.add_button("Send Logs").on_pressed([] {
            PartyBoard_SendLogs();
            push_toast({
                .type = "info",
                .title = "Send Logs",
#if defined(_WIN32)
                .content = "Your logs are being saved to Downloads. File Explorer opens next with the zip selected.",
#else
                .content = "Your logs are being saved to Downloads. A Share window opens next: pick who to send them to.",
#endif
                .duration = std::chrono::seconds(6),
            });
        }), rightPane, [](Pane &pane) { pane.add_text("Save recent logs to share when reporting a problem."); });
#endif
        config_bool_select(leftPane, rightPane, getSettings().game.enableControllerToasts,
            {
                .key = "Controller Notifications",
                .helpText = "Show controller connection notifications.",
            });
        config_bool_select(leftPane, rightPane, getSettings().backend.skipPreLaunchUI,
            {
                .key = "Skip Party Board Main Menu",
                .helpText = "Start the game directly when a disc image is set.",
            });
        config_bool_select(leftPane, rightPane, getSettings().backend.skipBootSequence,
            {
                .key = "Skip Game Boot Sequence",
                .helpText = "Skip the logos and title; start at mode select.",
            });
        // config_bool_select(leftPane, rightPane, getSettings().backend.showPipelineCompilation,
        //     {
        //         .key = "Show Pipeline Compilation",
        //         .helpText = "Show an overlay when shaders are being compiled for your hardware.",
        //     });
        config_bool_select(leftPane, rightPane, getSettings().game.pauseOnFocusLost,
            {
                .key = "Pause On Focus Lost",
                .helpText = "Pause the game when window focus is lost.",
                .onChange = [](bool value) { aurora_set_pause_on_focus_lost(value); },
            });
        // config_bool_select(leftPane, rightPane, getSettings().backend.enableAdvancedSettings,
        //     {
        //         .key = "Enable Advanced Settings",
        //         .icon = "warning",
        //         .helpText = "Show advanced settings and debugging tools with "
        //                     "Shift+F1.<br/><br/><icon class=\"warning\"/> WARNING: Debugging tools "
        //                     "can easily break your game. Do not use on a regular save!",
        //         .onChange =
        //             [](bool) {
        //                 for (auto &doc : get_document_stack()) {
        //                     if (dynamic_cast<MenuBar *>(doc.get())) {
        //                         doc = std::make_unique<MenuBar>();
        //                         break;
        //                     }
        //                 }
        //             },
        //     });

        // leftPane.add_section("Game");
        // config_bool_select(leftPane, rightPane, getSettings().game.recordingMode,
        //     {
        //         .key = "Recording Mode",
        //         .helpText = "Disables the game HUD and all background music.<br/><br/>Useful for "
        //                     "recording footage.",
        //     });
    });
}

void SettingsWindow::update()
{
    if (mPrelaunch && top_document() == this) {
        try_push_verification_modal(*this);
    }
#if defined(PARTYBOARD_HD_BETA)
    // Test aid (screenshots): PARTYBOARD_TEST_OPEN_UI=phone opens this window
    // (src/port/ui/ui.cpp); show the Input tab with Phone Controller selected.
    static const char *openUi = std::getenv("PARTYBOARD_TEST_OPEN_UI");
    if (openUi != nullptr && std::string_view(openUi) == "phone" && !mPrelaunch && mTestPhoneStep < 2) {
        if (mTestPhoneStep == 0 && set_active_tab(1)) {
            mTestPhoneStep = 1;
        } else if (mTestPhoneStep == 1 && mTestPhoneRow != nullptr && mTestPhoneRow->focus()) {
            mTestPhoneStep = 2;
        }
    }
    // =video: the Video tab with Window Mode selected.
    if (openUi != nullptr && std::string_view(openUi) == "video" && !mPrelaunch && mTestPhoneStep < 2) {
        // Video is the first tab, already showing (set_active_tab refuses it).
        if (mTestPhoneStep == 0) {
            mTestPhoneStep = 1;
        } else if (mTestPhoneStep == 1 && mTestVideoRow != nullptr && mTestVideoRow->focus()) {
            mTestPhoneStep = 2;
        }
    }
#endif

    Window::update();
}

} // namespace partyboard::ui
