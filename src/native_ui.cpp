// ProsperoAI native frontend, built with ps5-homebrew-ui: the frame, input and state.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "native_ui_impl.hpp"
#include "chatterbox_languages.hpp"
#include "runtime_settings.hpp"
#include "http_server.hpp"
#include "../third_party/qrcodegen/qrcodegen.hpp"

#include "debug_log.hpp"
#if defined(PROSPERO_UI_VULKAN) && !defined(PROSPERO_HOST)
#include "storage.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#if __has_include("prospero_build.h")
#include "prospero_build.h" // written by tools/build.sh from sce_sys/param.json
#endif
#ifndef PROSPERO_VERSION
#define PROSPERO_VERSION "development"
#define PROSPERO_BUILD_LABEL ""
#endif

namespace prospero
{
std::string logs_folder_text()
{
#if defined(PROSPERO_UI_VULKAN) && !defined(PROSPERO_HOST)
    return real_path(debug::folder());
#else
    return debug::folder()[0] ? debug::folder() : "/data/prosperoai/logs";
#endif
}

namespace
{
constexpr float kBootSeconds = 1.6f; // the opening is never shorter than its chime
constexpr const char *kTabs[] = {"Workspace", "Models", "Settings", "Mobile"};
constexpr const char *kCategories[] = {"Appearance", "Sound", "Accessibility", "Diagnostics",
                                       "About"};
enum DialogAction
{
    kUseModel = 1,
    kDeleteConversation,
    kDeleteModel,
    kCloseApp,
    kCancelDownload,
    kDownloadPreset,
    kDeleteVoice
};

std::string model_memory_note(const std::string &model_id)
{
    const auto settings = prospero_settings::get();
    const auto summary = debug::model_memory_summary(model_id.c_str(), settings.context,
                                                     settings.output_limit, settings.thinking);
    if (!summary.samples)
        return "No successful memory measurement exists for this model with the current context, "
               "output limit, and thinking setting. A reliable fit estimate is unavailable.";
    char note[320];
    constexpr double gib = 1024.0 * 1024.0 * 1024.0;
    std::snprintf(note, sizeof(note),
                  "%u past successful load(s). Largest observed Vulkan heap: %.2f GiB. "
                  "Lowest remaining flexible memory: %.2f GiB. Past runs do not guarantee "
                  "this model will fit now.",
                  summary.samples, summary.largest_vulkan_heap_bytes / gib,
                  summary.least_flexible_free_bytes / gib);
    return note;
}
} // namespace

NativeUI::Impl::Impl(NativeUI &owner, App &app, FontSet &font_set, gfx::Renderer &renderer)
    : owner_(owner), app_(app), font_set_(font_set), fonts_(font_set.fonts()), renderer_(renderer),
      palette_(make_palette(app.state().preferences)),
      theme_(make_theme(palette_, app.state().preferences))
{
    tabs_.style.kind = ui::TabKind::underline;
    tabs_.style.track = false;
    tabs_.style.focus_ring = false;
    tabs_.style.on_page = true;
    tabs_.style.text_size = 25;
    tabs_.style.gap = 38;
    tabs_.style.padding = 8;
    tabs_.style.height = 62;
    std::vector<ui::TabItem> tabs;
    tabs_width_ = 0;
    for (int i = 0; i < 4; ++i)
    {
        tabs.push_back({kTabs[i], 0, false, i});
        tabs_width_ += fonts_.semibold.measure(kTabs[i], 25) + 16 + (i ? 38.0f : 0.0f);
    }
    tabs_.set_tabs(std::move(tabs));
    // The row sits in the middle of the header, between the L1 and R1 glyphs.
    tabs_left_ = 960 - tabs_width_ * 0.5f;
    tabs_.set_bounds({tabs_left_, 57, tabs_width_ + 8, 66});
    tabs_.set_focused(false);

    sessions_.style.title_size = 24;
    sessions_.style.subtitle_size = 19;
    sessions_.style.row_height = 86;
    sessions_.style.gap = 8;
    sessions_.style.padding = 20;
    sessions_.style.highlight.kind = ui::HighlightKind::bar;
    sessions_.set_bounds({110, 322, 334, 598});
    sessions_.set_active(false);

    models_.style.columns = 3;
    models_.style.cell_height = 196;
    models_.style.gap_x = 24;
    models_.style.padding = 14;
    models_.style.card.text = ui::CardText::none;
    models_.style.card.art_aspect = 0;
    models_.style.card.focus_scale = 1.025f;
    models_.style.card.lift = 6;
    models_.style.card.glow = false;
    models_.set_bounds({82, 474, 1756, 456});
    models_.content =
        [this](ui::Canvas &canvas, const Rect &cell, const ui::CardItem &, int index, float focus)
    { draw_model(canvas.list, cell, visible_models_[static_cast<std::size_t>(index)], focus); };

    filters_.style.kind = ui::TabKind::segmented;
    filters_.style.width = ui::TabWidth::fill;
    filters_.style.height = 60;
    filters_.style.text_size = 23;
    filters_.style.wrap = true;
    filters_.set_tabs({{"Download", 0, false, 5},
                       {"All", 0, false, 0},
                       {"Text", 0, false, 1},
                       {"Image", 0, false, 2},
                       {"Audio", 0, false, 3},
                       {"Voice", 0, false, 4}});
    filters_.set_active(1);
    filters_.set_bounds({732, 392, 1092, 60});
    filters_.set_focused(false);
    search_.style.max_rows = 0;
    search_.set_bounds({kLeft, 392, 612, 60});
    search_.set_placeholder("Search your models");
    // Square deletes, Triangle is space, L1 is shift, L2 switches between letters and
    // symbols, R2 is Done (Send in a conversation). R2 was also the kit's symbols key, so one
    // press did both and there was no way back to letters.
    keyboard_.style.bindings = ui::KeyboardBindings::standard();
    keyboard_.style.bindings.space = Action::north;
    keyboard_.style.bindings.shift = Action::page_prev;
    keyboard_.style.bindings.layout = Action::jump_prev;
    keyboard_.style.bindings.done = Action::jump_next;
    keyboard_.style.max_length = 1023;
    keyboard_.set_bounds({360, 596, 1200, 310});
    keyboard_.on_text = [this](std::string_view value)
    {
        if (chat_keyboard_open_)
            composer_.insert(value);
        else if (tabs_.active() == 0)
        {
            if (selecting_model_)
                workspace_model_query_ += value;
            else
            {
                session_search_.insert(value);
                session_query_ = session_search_.text();
            }
            refresh_sessions();
        }
        else
        {
            search_.insert(value);
            refresh_models();
        }
    };
    keyboard_.on_backspace = [this]
    {
        if (chat_keyboard_open_)
            composer_.backspace();
        else if (tabs_.active() == 0)
        {
            if (selecting_model_)
            {
                if (!workspace_model_query_.empty())
                    workspace_model_query_.pop_back();
            }
            else
            {
                session_search_.backspace();
                session_query_ = session_search_.text();
            }
            refresh_sessions();
        }
        else
        {
            search_.backspace();
            refresh_models();
        }
    };

    form_.style.row_height = 88;
    form_.style.gap = 10;
    form_.style.label_size = 26;
    form_.style.value_size = 24;
    form_.style.control_width = 300;
    form_.style.highlight.kind = ui::HighlightKind::ring;
    form_.style.description_inline = false;
    form_.style.dividers = true;
    form_.style.padding = 26;
    form_.set_bounds({494, 362, 912, 520});

    dialog_.style.width = 830;
    dialog_.style.title_size = 40;
    dialog_.style.body_size = 27;
    dialog_.style.icon_size = 0;
    dialog_.style.padding = 48;
    dialog_.style.scrim = 0.62f;
    dialog_.style.frost = 0.78f;
    dialog_.style.centered = false;
    setup_update();

    composer_.set_bounds({536, 850, 1288, 72});
    composer_.style.field_height = 72;
    composer_.style.max_length = 1023;
    composer_.style.counter = false;
    composer_.set_placeholder("Take an idea a little further...");
    chat_.set_bounds({536, 372, 1272, 400});
    chat_.style.clip_bleed = 0;

    categories_.style = sessions_.style;
    categories_.style.row_height = 76;
    categories_.style.title_size = 25;
    categories_.set_bounds({110, 362, 322, 520});
    std::vector<ui::ListItem> categories;
    for (const char *name : kCategories)
    {
        ui::ListItem item;
        item.title = name;
        categories.push_back(std::move(item));
    }
    categories_.set_items(std::move(categories));
    categories_.set_active(false);

    toasts_.style.anchor = ui::ToastAnchor::top_right;
    toasts_.style.margin = 96;
    toasts_.style.width = 500;
    toasts_.set_bounds({0, 78, gfx::kVirtualWidth, 900});

    apply_theme();
    page_top_.snap(palette_.page_top);
    page_bottom_.snap(palette_.page_bottom);
    cloud_a_.snap(palette_.page_bottom);
    cloud_b_.snap(palette_.page_bottom);
    refresh_models();
    build_form();
}

void NativeUI::Impl::apply_theme()
{
    const auto &p = app_.state().preferences;
    palette_ = make_palette(p);
    theme_ = make_theme(palette_, p);
    const std::initializer_list<ui::ComponentStyle *> styles{
        &tabs_.style,          &filters_.style,     &sessions_.style,    &categories_.style,
        &models_.style,        &search_.style,      &keyboard_.style,    &form_.style,
        &dialog_.style,        &composer_.style,    &chat_.style,        &toasts_.style,
        &update_dialog_.style, &update_ring_.style, &update_notes_.style};
    for (ui::ComponentStyle *style : styles)
    {
        style->theme = theme_;
        style->reduced_motion = p.reduced_motion;
    }
    sessions_.style.highlight.color = categories_.style.highlight.color =
        palette_.accent.with_alpha(0.75f);
}

void NativeUI::Impl::change_page(int page)
{
    if (page != 0 && workspace_generation_open_)
    {
        workspace_generation_open_ = false;
        build_form();
    }
    tabs_.set_active(page);
    if (page == 2)
        build_form();
    if (page == 3)
        refresh_mobile_link();
    page_age_ = 0;
    hero_age_ = 0;
    models_.enter();
    form_.enter();
    sessions_.enter();
}

void NativeUI::Impl::refresh_mobile_link()
{
    char url[128]{};
    mobile_qr_modules_.clear();
    mobile_qr_size_ = 0;
    mobile_url_.clear();
    if (!prospero_http_server_mobile_url(url, sizeof(url)))
        return;
    try
    {
        const auto qr = qrcodegen::QrCode::encodeText(url, qrcodegen::QrCode::Ecc::MEDIUM);
        mobile_qr_size_ = qr.getSize();
        mobile_qr_modules_.resize(static_cast<std::size_t>(mobile_qr_size_ * mobile_qr_size_));
        for (int y = 0; y < mobile_qr_size_; ++y)
            for (int x = 0; x < mobile_qr_size_; ++x)
                mobile_qr_modules_[static_cast<std::size_t>(y * mobile_qr_size_ + x)] =
                    qr.getModule(x, y) ? 1 : 0;
        mobile_url_ = url;
    }
    catch (const std::exception &)
    {
        mobile_qr_modules_.clear();
        mobile_qr_size_ = 0;
        mobile_url_.clear();
    }
}

void NativeUI::Impl::announce(ui::Feedback &feedback)
{
    for (const Notice notice : app_.take_notices())
    {
        const auto &state = app_.state();
        const bool watching = tabs_.active() == 0 && conversation_;
        switch (notice)
        {
        case Notice::ModelReady:
            pending_model_ = -1;
            selecting_model_ = false;
            conversation_ = true;
            rail_ = false;
            change_page(0);
            refresh_sessions();
            toasts_.push(ui::StatusKind::success, std::string(current_name()) + " is ready",
                         "A new conversation starts with this model.");
            break;
        case Notice::ModelFailed:
            pending_model_ = -1;
            if (tabs_.active() == 0)
                begin_model_selection();
            toasts_.push(ui::StatusKind::danger, "The model could not be prepared",
                         "Choose it again in Your Space to retry.", 6);
            break;
        case Notice::ModelMissing:
            toasts_.push(ui::StatusKind::warning, "This model is not installed",
                         "The conversation can be read, but not continued.", 6);
            break;
        case Notice::ReplyReady:
            if (watching)
                feedback.play(audio::Cue::notify, 1.12f, 0.0f, 0.8f);
            else
                toasts_.push(ui::StatusKind::success, "Your answer is ready", state.session.title);
            break;
        case Notice::MediaReady:
            if (watching)
                feedback.play(audio::Cue::complete);
            else
                toasts_.push(ui::StatusKind::success,
                             state.stats_kind == Capability::Image ? "Your image is ready"
                                                                   : "Your audio is ready",
                             state.session.title);
            break;
        case Notice::GenerationFailed:
            toasts_.push(ui::StatusKind::danger, "That did not work", state.status, 6);
            break;
        case Notice::SaveFailed:
            toasts_.push(ui::StatusKind::warning, "Not saved yet", state.status, 6);
            break;
        case Notice::ConversationDeleted:
            toasts_.push(ui::StatusKind::info, "Conversation deleted");
            break;
        case Notice::SettingsNotSaved:
            toasts_.push(ui::StatusKind::warning, "Settings not saved",
                         "Your changes apply until ProsperoAI closes.", 6);
            break;
        case Notice::None:
            break;
        }
    }
}

void NativeUI::Impl::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    prospero_model_download::poll();
    const auto download_state = prospero_model_download::state();
    if (download_state == prospero_model_download::State::Complete &&
        last_download_state_ != download_state)
        downloaded_models_pending_ = true;
    last_download_state_ = download_state;
    if (cancel_pending_ && download_state != prospero_model_download::State::Downloading)
    {
        cancel_pending_ = false;
        // It may have finished in the moment before the request reached it.
        if (download_state != prospero_model_download::State::Complete)
            toasts_.push(ui::StatusKind::info, "Download cancelled", "Nothing of it was kept.");
    }
    if (downloaded_models_pending_ && app_.refresh_models())
    {
        downloaded_models_pending_ = false;
        refresh_models();
    }
    if (prospero_model_download::state() != prospero_model_download::State::Loading)
        search_.set_busy(false);
    const bool downloading_models = filters_.active() == 0;
    search_.set_placeholder(downloading_models ? "Search model names (e.g. Mistral 7B)"
                                               : "Search your models");
    search_.set_bounds(downloading_models ? Rect{128, 558, 1056, 64} : Rect{kLeft, 392, 612, 60});
    filters_.set_bounds((downloading_models || app_.state().models.empty())
                            ? Rect{kLeft + 64, 392, 1600, 60}
                            : Rect{796, 392, 964, 60});
    const bool was_generating = app_.generating();
    app_.poll();
    session_sync_age_ += dt;
    if (session_sync_age_ >= 4.0f)
    {
        session_sync_age_ = 0;
        app_.sync_sessions();
    }
    if (!welcomed_)
    {
        welcomed_ = true;
        feedback.play(audio::Cue::welcome);
    }
    announce(feedback);
    // Faces for scripts first seen on the last frame are read here, once.
    font_set_.load_pending();
    if (revision_ != app_.state().revision || font_revision_ != font_set_.revision())
        sync();
    upload_visible_image();

    clock_ += reduced_motion() ? 0 : dt;
    page_age_ += dt;
    hero_age_ += dt;
    boot_age_ += dt;
    if (app_.generating())
        busy_seconds_ = was_generating ? busy_seconds_ + dt : 0;
    for (auto &row : chat_rows_)
        row.age += dt;
    send_pulse_.update(dt, 4);

    // The opening covers the first moments: the catalogue is read behind it.
    const bool booting = !app_.state().initialized || boot_age_ < kBootSeconds;
    const float boot_step = dt / (reduced_motion() ? 0.15f : 0.5f);
    boot_ = std::clamp(boot_ + (booting ? boot_step : -boot_step), 0.0f, 1.0f);

    // The backdrop takes the accent and the colour of what is in focus.
    const auto &state = app_.state();
    Capability hue = Capability::Text;
    if (tabs_.active() == 1 && !visible_models_.empty())
    {
        const int index = visible_models_[static_cast<std::size_t>(models_.focus())];
        hue = index >= 0
                  ? state.models[static_cast<std::size_t>(index)].capability
                  : static_cast<Capability>(prospero_model_download::presets[-index - 2].kind);
    }
    else if (state.selected_model >= 0)
        hue = state.models[static_cast<std::size_t>(state.selected_model)].capability;
    // A conversation model's own colour is the accent, so the second cloud is a cool
    // tone instead: the page keeps a warm side and a deep side.
    const Color second = hue == Capability::Text ? palette_.depth : kind_color(hue);
    cloud_a_.target(gfx::mix(palette_.page_bottom, palette_.accent, palette_.day ? 0.16f : 0.20f));
    cloud_b_.target(gfx::mix(palette_.page_bottom, second, palette_.day ? 0.30f : 0.24f));
    page_top_.target(palette_.page_top);
    page_bottom_.target(palette_.page_bottom);
    const float colour_speed = reduced_motion() ? 60.0f : 4.0f;
    for (ui::SpringColor *colour : {&cloud_a_, &cloud_b_, &page_top_, &page_bottom_})
        colour->update(dt, colour_speed);

    // A newer version waits for a quiet moment: nothing running, no other question open.
    if (update_ui_ == UpdateUi::hidden && boot_ < 0.01f && !keyboard_pending_ && !search_open_ &&
        !chat_keyboard_open_ && !dialog_.is_open() && !app_.busy())
        take_update_offer(feedback);

    if (!voice_recording_open_ && voice_recorder_.state() == VoiceRecorder::State::Recording)
    {
        voice_recording_open_ = true;
        voice_recording_remote_ = true;
        voice_recording_feedback_.clear();
    }
    else if (voice_recording_remote_ && voice_recorder_.state() == VoiceRecorder::State::Idle)
    {
        voice_recording_open_ = false;
        voice_recording_remote_ = false;
    }
    if (boot_ > 0.6f || keyboard_pending_)
    {
        // Nothing is in reach yet.
    }
    else if (update_ui_ != UpdateUi::hidden)
        update_modal(input, dt, feedback);
    else if (voice_recording_open_)
    {
        const auto state = voice_recorder_.state();
        if (input.is_pressed(Action::back))
        {
            if (!app_.busy())
            {
                voice_recorder_.discard();
                voice_recording_open_ = false;
            }
        }
        else if (!app_.busy() && input.is_pressed(Action::confirm))
        {
            if (state == VoiceRecorder::State::Recording)
                voice_recorder_.stop();
            else if (state == VoiceRecorder::State::Ready)
            {
                if (voice_recorder_.save())
                {
                    auto preferences = app_.state().preferences;
                    const int index = prospero_voice::runtime_voice_index(true, "custom");
                    if (index >= 0)
                        preferences.chatterbox_voice = static_cast<unsigned>(index);
                    app_.set_preferences(std::move(preferences));
                    voice_recorder_.discard();
                    voice_recording_open_ = false;
                    kokoro_voice_picker_open_ = false;
                    refresh_sessions();
                    toasts_.push(ui::StatusKind::success, "Voice saved",
                                 "My voice is selected for Chatterbox.");
                }
                else
                    voice_recording_feedback_ = voice_recorder_.error();
            }
            else
            {
                voice_recording_feedback_.clear();
                voice_recorder_.start();
            }
        }
        else if (!app_.busy() && state == VoiceRecorder::State::Ready &&
                 input.is_pressed(Action::west))
        {
            if (!voice_recorder_.preview() || !app_.play_audio(VoiceRecorder::preview_path))
                voice_recording_feedback_ = "Could not play the voice sample.";
        }
        else if (!app_.busy() && state != VoiceRecorder::State::Recording &&
                 input.is_pressed(Action::north))
        {
            voice_recording_feedback_.clear();
            voice_recorder_.start();
        }
    }
    else if (kokoro_voice_picker_open_)
    {
        if (voice_picker_chatterbox_ && input.is_pressed(Action::l3) && !app_.busy())
        {
            const auto selected = prospero_voice::runtime_voice(
                true, static_cast<unsigned>(kokoro_voice_picker_focus_));
            if (selected && prospero_voice::chatterbox_voice_deletable(*selected))
            {
                voice_delete_id_ = selected->id;
                kokoro_voice_picker_open_ = false;
                open_dialog(kDeleteVoice, -1, "Delete " + selected->name + "?",
                            "This voice sample will be removed from this console. You can record "
                            "or upload it again.",
                            "Delete", true, feedback, "Cancel");
            }
            else
                toasts_.push(ui::StatusKind::info, "Built-in voice",
                             "Only your own and imported samples can be deleted.");
        }
        else if (voice_picker_chatterbox_ && input.is_pressed(Action::north) && !app_.busy())
        {
            voice_recording_open_ = true;
            voice_recording_remote_ = false;
            voice_recording_feedback_.clear();
            feedback.play(audio::Cue::modal_open);
        }
        else if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int count =
                static_cast<int>(prospero_voice::runtime_voices(voice_picker_chatterbox_).size());
            kokoro_voice_picker_focus_ =
                (kokoro_voice_picker_focus_ + (input.nav == Direction::down ? 1 : count - 1)) %
                count;
            feedback.play(audio::Cue::focus);
        }
        else if (input.is_pressed(Action::back))
        {
            kokoro_voice_picker_open_ = false;
            feedback.play(audio::Cue::modal_close);
        }
        else if (input.is_pressed(Action::confirm))
        {
            auto preferences = app_.state().preferences;
            if (voice_picker_chatterbox_)
                preferences.chatterbox_voice = static_cast<unsigned>(kokoro_voice_picker_focus_);
            else
                preferences.kokoro_voice = static_cast<unsigned>(kokoro_voice_picker_focus_);
            app_.set_preferences(std::move(preferences));
            kokoro_voice_picker_open_ = false;
            refresh_sessions();
            feedback.play(audio::Cue::select);
        }
    }
    else if (language_picker_open_)
    {
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int count = static_cast<int>(prospero_language::chatterbox_count);
            language_picker_focus_ =
                (language_picker_focus_ + (input.nav == Direction::down ? 1 : count - 1)) % count;
            feedback.play(audio::Cue::focus);
        }
        else if (input.is_pressed(Action::back))
        {
            language_picker_open_ = false;
            feedback.play(audio::Cue::modal_close);
        }
        else if (input.is_pressed(Action::confirm))
        {
            auto preferences = app_.state().preferences;
            preferences.chatterbox_language = static_cast<unsigned>(language_picker_focus_);
            app_.set_preferences(std::move(preferences));
            language_picker_open_ = false;
            refresh_sessions();
            feedback.play(audio::Cue::select);
        }
    }
    else if (chat_keyboard_open_)
    {
        const auto event = keyboard_.handle(input, feedback);
        if (event == ui::Event::activated || event == ui::Event::cancelled)
        {
            chat_keyboard_open_ = false;
            feedback.play(audio::Cue::modal_close);
            if (event == ui::Event::activated && !composer_.text().empty())
                submit(feedback);
        }
    }
    else if (search_open_)
    {
        const auto event = keyboard_.handle(input, feedback);
        if (event == ui::Event::activated || event == ui::Event::cancelled)
        {
            search_open_ = false;
            search_.set_active(false);
            feedback.play(audio::Cue::modal_close);
        }
    }
    else if (dialog_.is_open())
    {
        const auto event = dialog_.handle(input, feedback);
        if (dialog_action_ == kDeleteVoice &&
            (event == ui::Event::cancelled ||
             (event == ui::Event::activated && dialog_.choice() != 1)))
            kokoro_voice_picker_open_ = true;
        if (event == ui::Event::activated && dialog_.choice() == 1)
        {
            bool accepted = false;
            if (dialog_action_ == kUseModel)
            {
                accepted = app_.select_model(static_cast<unsigned>(dialog_index_));
                if (accepted)
                    pending_model_ = dialog_index_;
            }
            else if (dialog_action_ == kDeleteConversation)
                accepted = app_.delete_session(static_cast<unsigned>(dialog_index_));
            else if (dialog_action_ == kDeleteModel)
            {
                const auto &models = app_.state().models;
                accepted = dialog_index_ >= 0 &&
                           static_cast<std::size_t>(dialog_index_) < models.size() &&
                           app_.state().selected_model != dialog_index_ &&
                           prospero_model_download::delete_model_files(
                               models[static_cast<std::size_t>(dialog_index_)].id.c_str());
                if (accepted)
                {
                    app_.refresh_models();
                    feedback.play(audio::Cue::tab);
                }
            }
            else if (dialog_action_ == kCloseApp)
                accepted = quit_ = !app_.busy() && !app_.state().unsaved;
            else if (dialog_action_ == kCancelDownload)
                accepted = cancel_pending_ = prospero_model_download::cancel();
            else if (dialog_action_ == kDownloadPreset && dialog_index_ >= 0 &&
                     static_cast<std::size_t>(dialog_index_) <
                         prospero_model_download::preset_count)
            {
                const auto &preset = prospero_model_download::presets[dialog_index_];
                accepted = prospero_model_download::download_preset(
                    static_cast<std::size_t>(dialog_index_));
                if (accepted)
                    toasts_.push(ui::StatusKind::info, "Downloading model", preset.name);
            }
            else if (dialog_action_ == kDeleteVoice)
            {
                auto preferences = app_.state().preferences;
                const auto current =
                    prospero_voice::runtime_voice(true, preferences.chatterbox_voice);
                accepted = prospero_voice::delete_chatterbox_voice(voice_delete_id_);
                if (accepted)
                {
                    const std::string next =
                        current && current->id != voice_delete_id_ ? current->id : "cemo";
                    const int index = prospero_voice::runtime_voice_index(true, next);
                    preferences.chatterbox_voice = static_cast<unsigned>(std::max(0, index));
                    app_.set_preferences(std::move(preferences));
                    kokoro_voice_picker_focus_ =
                        static_cast<int>(app_.state().preferences.chatterbox_voice);
                    refresh_sessions();
                    toasts_.push(ui::StatusKind::success, "Voice sample deleted",
                                 "Your conversations remain on this console.");
                }
                else
                    toasts_.push(ui::StatusKind::warning, "Voice not deleted",
                                 "The sample is missing or could not be removed.");
                kokoro_voice_picker_open_ = true;
            }
            if (!accepted)
                feedback.play(audio::Cue::error);
        }
    }
    else if (input.is_pressed(Action::menu))
    {
        if (app_.busy() || state.unsaved)
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.7f);
            toasts_.push(ui::StatusKind::info,
                         app_.busy() ? "Still working" : "This conversation is not saved yet",
                         app_.busy() ? "ProsperoAI can close once this is finished."
                                     : "Retry the save before closing.");
        }
        else
            open_dialog(kCloseApp, -1, "Close ProsperoAI?",
                        "Your conversations stay on this console.", "Close", false, feedback);
    }
    else if (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev))
    {
        if (conversation_ && selecting_model_ && rail_)
        {
            const int delta = input.is_pressed(Action::page_next) ? 1 : -1;
            workspace_model_filter_ = (workspace_model_filter_ + 1 + delta + 5) % 5 - 1;
            sessions_.set_focus(0);
            refresh_sessions();
            feedback.play(audio::Cue::focus);
        }
        else if (tabs_.step(input.is_pressed(Action::page_next) ? 1 : -1, input, feedback) ==
                 ui::Event::changed)
            change_page(tabs_.active());
    }
    else if (tabs_.active() == 1)
        handle_models(input, feedback);
    else if (tabs_.active() == 2)
        handle_settings(input, feedback);
    else if (tabs_.active() == 3)
    {
        // The mobile page is informational; the R1/L1 tabs remain its only controls.
    }
    else
        handle_workspace(input, feedback);

    // The model under the focus leads the Models page; a new one arrives with a short fade.
    const int hero = tabs_.active() == 1 && !visible_models_.empty()
                         ? visible_models_[static_cast<std::size_t>(models_.focus())]
                         : -1;
    if (hero != hero_model_)
    {
        hero_model_ = hero;
        hero_age_ = 0;
    }

    sessions_.set_active(rail_);
    categories_.set_active(category_focus_);
    form_.set_active(workspace_generation_open_ || (tabs_.active() == 2 && !category_focus_));
    composer_.set_active(!rail_ && !app_.busy());
    composer_.set_disabled(app_.busy());
    tabs_.update(dt);
    sessions_.update(dt);
    categories_.update(dt);
    models_.update(dt);
    filters_.update(dt);
    search_.update(dt);
    keyboard_.update(dt);
    form_.update(dt);
    dialog_.update(dt);
    update_tick(dt);
    composer_.update(dt);
    chat_.update(dt);
    toasts_.update(dt, feedback);
}

void NativeUI::Impl::handle_models(const InputFrame &input, ui::Feedback &feedback)
{
    if (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next))
    {
        const int previous_filter = filters_.active();
        filters_.step(input.is_pressed(Action::jump_next) ? 1 : -1, input, feedback);
        if (previous_filter == 0 || filters_.active() == 0)
            search_.clear();
        filters_.set_focused(false);
        download_focus_ = 0;
        refresh_models();
        return;
    }
    const auto state = prospero_model_download::state();
    const bool has_download_results = prospero_model_download::candidate_count() > 0 &&
                                      (state == prospero_model_download::State::Ready ||
                                       state == prospero_model_download::State::SearchReady);
    const auto open_search = [&]
    {
        search_open_ = true;
        search_.set_active(true);
        keyboard_.style.max_length = filters_.active() == 0 ? 128 : 64;
        keyboard_.set_length(search_.length());
        keyboard_.enter();
        feedback.play(audio::Cue::modal_open);
    };
    // A running download is cancelled from anywhere on the page, after a confirmation.
    const auto ask_cancel = [&]
    {
        if (!prospero_model_download::cancelling())
            open_dialog(kCancelDownload, -1, "Cancel this download?",
                        "What has been downloaded so far is removed.", "Cancel download", true,
                        feedback, "Keep downloading");
    };
    if (state == prospero_model_download::State::Downloading && input.is_pressed(Action::north))
    {
        ask_cancel();
        return;
    }

    if (filters_.active() == 0)
    {
        if (input.is_pressed(Action::back))
        {
            filters_.set_active(1);
            refresh_models();
            feedback.play(audio::Cue::tab);
            return;
        }
        if (input.is_pressed(Action::west))
            open_search();
        else if (input.nav == Direction::up && has_download_results)
            download_focus_ = std::max(0, download_focus_ - 1);
        else if (input.nav == Direction::down && has_download_results)
            download_focus_ =
                std::min(static_cast<int>(prospero_model_download::candidate_count()) - 1,
                         download_focus_ + 1);
        else if (input.is_pressed(Action::confirm))
        {
            if (state == prospero_model_download::State::Downloading)
            {
                ask_cancel();
                return;
            }
            if (!has_download_results && (state == prospero_model_download::State::Ready ||
                                          state == prospero_model_download::State::SearchReady))
            {
                open_search();
                return;
            }
            if (state == prospero_model_download::State::Ready)
            {
                if (!prospero_model_download::download(static_cast<std::size_t>(download_focus_)))
                    feedback.play(audio::Cue::error);
            }
            else if (state == prospero_model_download::State::SearchReady)
            {
                prospero_model_download::Candidate candidate{};
                if (prospero_model_download::candidate(static_cast<std::size_t>(download_focus_),
                                                       &candidate) &&
                    prospero_model_download::browse(candidate.name))
                    search_.set_busy(true);
                else
                    feedback.play(audio::Cue::error);
            }
            else if (state != prospero_model_download::State::Loading &&
                     state != prospero_model_download::State::Searching &&
                     state != prospero_model_download::State::Downloading)
            {
                if (search_.length() < 2)
                {
                    open_search();
                    return;
                }
                if (prospero_model_download::search(search_.text().c_str()))
                {
                    download_focus_ = 0;
                    search_.set_busy(true);
                }
                else
                    feedback.play(audio::Cue::error);
            }
        }
        return;
    }
    if (input.is_pressed(Action::west))
        open_search();
    else if (input.is_pressed(Action::l3) && !visible_models_.empty())
    {
        const int index = visible_models_[static_cast<std::size_t>(models_.focus())];
        if (index >= 0 && static_cast<std::size_t>(index) < app_.state().models.size())
        {
            const auto &model = app_.state().models[static_cast<std::size_t>(index)];
            if (index == app_.state().selected_model)
                toasts_.push(ui::StatusKind::info, "Model is in use",
                             "Choose another model before deleting this one.");
            else
                open_dialog(kDeleteModel, index, "Delete this model?",
                            model.name + " and its installed files will be removed from the "
                                         "console.",
                            "Delete model", true, feedback);
        }
        else
            feedback.play(audio::Cue::error);
    }
    else if (models_.handle(input, feedback) == ui::Event::activated && !visible_models_.empty())
    {
        const int index = visible_models_[static_cast<std::size_t>(models_.focus())];
        const auto &state = app_.state();
        if (index < -1)
        {
            const auto preset_index = static_cast<std::size_t>(-index - 2);
            if (prospero_model_download::active_preset() == static_cast<int>(preset_index))
            {
                ask_cancel();
                return;
            }
            if (preset_saved_[preset_index])
            {
                toasts_.push(ui::StatusKind::info, "Model bundle saved",
                             "The model is installed in your library.");
            }
            else
            {
                const auto &preset = prospero_model_download::presets[preset_index];
                const auto size_mib =
                    static_cast<unsigned long long>(preset.size / (1024ULL * 1024ULL));
                open_dialog(kDownloadPreset, static_cast<int>(preset_index), "Download this model?",
                            std::string(preset.name) + " requires " + std::to_string(size_mib) +
                                " MiB. Free console storage cannot be checked safely. Check that "
                                "enough space is available before continuing.",
                            "Download model", false, feedback);
            }
            return;
        }
        if (app_.busy() || state.unsaved)
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.7f);
            toasts_.push(ui::StatusKind::info,
                         app_.busy() ? "Still working" : "This conversation is not saved yet",
                         app_.busy() ? "Choose a model once this is finished."
                                     : "Retry the save in Workspace first.");
        }
        else
            open_dialog(kUseModel, index,
                        "Use " + state.models[static_cast<std::size_t>(index)].name + "?",
                        "Your current conversation is kept. A new conversation starts with "
                        "this model. " +
                            model_memory_note(state.models[static_cast<std::size_t>(index)].id),
                        "Use model", false, feedback);
    }
}

void NativeUI::Impl::handle_settings(const InputFrame &input, ui::Feedback &feedback)
{
    if (input.is_pressed(Action::back) && !category_focus_)
    {
        category_focus_ = true;
        feedback.play(audio::Cue::back);
    }
    else if (category_focus_)
    {
        const auto event = categories_.handle(input, feedback);
        if (event == ui::Event::moved)
        {
            category_ = categories_.focus();
            build_form();
        }
        if (event == ui::Event::activated || input.nav == Direction::right)
            category_focus_ = false;
    }
    else if (form_.handle(input, feedback) == ui::Event::changed)
        apply_form();
}

int NativeUI::Impl::selected_audio_row() const
{
    const float top = chat_.offset_y();
    const float bottom = top + chat_.bounds().h;
    const float center = top + chat_.bounds().h * 0.5f;
    int selected = -1;
    float closest_distance = 0;
    for (std::size_t index = 0; index < chat_rows_.size(); ++index)
    {
        const auto &row = chat_rows_[index];
        if (!row.audio || row.audio_path.empty() || row.y + row.height < top || row.y > bottom)
            continue;
        const float distance = std::abs(row.y + row.height * 0.5f - center);
        if (selected < 0 || distance < closest_distance)
        {
            selected = static_cast<int>(index);
            closest_distance = distance;
        }
    }
    return selected;
}

void NativeUI::Impl::handle_workspace(const InputFrame &input, ui::Feedback &feedback)
{
    if (workspace_generation_open_)
    {
        if (input.is_pressed(Action::back))
        {
            workspace_generation_open_ = false;
            build_form();
            feedback.play(audio::Cue::back);
        }
        else if (form_.handle(input, feedback) == ui::Event::changed)
            apply_generation_form();
        return;
    }
    chat_.handle(input, feedback);
    if (rail_ && (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next)))
    {
        session_category_ = (session_category_ + (input.is_pressed(Action::jump_next) ? 1 : 4)) % 5;
        refresh_sessions();
    }
    else if (rail_ && input.is_pressed(Action::west))
    {
        search_open_ = true;
        keyboard_.style.max_length = selecting_model_ ? 96 : 64;
        keyboard_.set_length(selecting_model_ ? workspace_model_query_.size()
                                              : session_search_.length());
        keyboard_.enter();
    }
    else if (input.is_pressed(Action::west))
    {
        if (app_.new_session())
        {
            begin_model_selection();
            feedback.play(audio::Cue::open);
        }
        else
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.7f);
    }
    else if (!conversation_)
    {
        if (input.is_pressed(Action::confirm))
        {
            conversation_ = rail_ = true;
            page_age_ = 0;
            refresh_sessions();
            feedback.play(audio::Cue::open);
        }
        else if (input.nav == Direction::left)
        {
            conversation_ = rail_ = true;
            page_age_ = 0;
            refresh_sessions();
            feedback.play(audio::Cue::open);
        }
    }
    else if (input.nav == Direction::left && !rail_)
    {
        rail_ = true;
        feedback.play(audio::Cue::focus, 0.94f, -0.4f);
    }
    else if (rail_ && selecting_model_ && input.is_pressed(Action::back))
    {
        selecting_model_ = false;
        refresh_sessions();
        feedback.play(audio::Cue::back);
    }
    else if (rail_ && (input.nav == Direction::right ||
                       (!selecting_model_ && input.is_pressed(Action::back))))
    {
        rail_ = false;
        feedback.play(audio::Cue::focus, 1.0f, 0.2f);
    }
    else if (rail_)
    {
        if (input.is_pressed(Action::l3) && selecting_model_ && sessions_.focus() >= 0 &&
            static_cast<std::size_t>(sessions_.focus()) < workspace_model_indices_.size())
        {
            const unsigned index =
                workspace_model_indices_[static_cast<std::size_t>(sessions_.focus())];
            const auto &model = app_.state().models[index];
            if (static_cast<int>(index) == app_.state().selected_model)
                toasts_.push(ui::StatusKind::info, "Model is in use",
                             "Choose another model before deleting this one.");
            else
                open_dialog(kDeleteModel, static_cast<int>(index), "Delete this model?",
                            model.name + " and its installed files will be removed from the "
                                         "console.",
                            "Delete model", true, feedback);
        }
        else if (input.is_pressed(Action::l3) && !selecting_model_ && sessions_.focus() == 0 &&
                 app_.state().selected_model >= 0)
        {
            begin_model_selection();
            feedback.play(audio::Cue::open);
        }
        else if (input.is_pressed(Action::l3) && !selecting_model_ &&
                 sessions_.focus() >= workspace_sessions_row_ &&
                 static_cast<std::size_t>(sessions_.focus() - workspace_sessions_row_) <
                     visible_sessions_.size())
        {
            if (app_.busy())
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.7f);
            else
                open_dialog(kDeleteConversation,
                            visible_sessions_[static_cast<std::size_t>(sessions_.focus() -
                                                                       workspace_sessions_row_)],
                            "Delete this conversation?",
                            "The conversation will be removed. Generated image, audio, and "
                            "voice files will remain on this console.",
                            "Delete", true, feedback);
        }
        else if (sessions_.handle(input, feedback) == ui::Event::activated)
        {
            if (selecting_model_)
            {
                if (sessions_.focus() >= 0 &&
                    static_cast<std::size_t>(sessions_.focus()) < workspace_model_indices_.size())
                {
                    const unsigned index =
                        workspace_model_indices_[static_cast<std::size_t>(sessions_.focus())];
                    const auto &model = app_.state().models[index];
                    open_dialog(kUseModel, static_cast<int>(index), "Use " + model.name + "?",
                                "This model will load into memory for the new conversation. " +
                                    model_memory_note(model.id),
                                "Use model", false, feedback);
                }
                else
                    feedback.play(audio::Cue::error);
            }
            else if (sessions_.focus() == 0)
                begin_model_selection();
            else if (sessions_.focus() == 1)
            {
                workspace_generation_open_ = true;
                build_generation_form();
                form_.enter();
                feedback.play(audio::Cue::open);
            }
            else if (sessions_.focus() == workspace_new_conversation_row_ &&
                     workspace_new_conversation_row_ >= 0)
            {
                if (app_.new_session())
                    begin_model_selection();
                else
                    feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.7f);
            }
            else if (sessions_.focus() == workspace_language_row_ && workspace_language_row_ >= 0)
            {
                language_picker_focus_ =
                    static_cast<int>(app_.state().preferences.chatterbox_language);
                language_picker_open_ = true;
                feedback.play(audio::Cue::modal_open);
            }
            else if (sessions_.focus() == workspace_voice_row_ && workspace_voice_row_ >= 0)
            {
                const auto &model =
                    app_.state().models[static_cast<std::size_t>(app_.state().selected_model)];
                voice_picker_chatterbox_ = model.id.find("chatterbox") != std::string::npos;
                kokoro_voice_picker_focus_ = static_cast<int>(
                    voice_picker_chatterbox_ ? app_.state().preferences.chatterbox_voice
                                             : app_.state().preferences.kokoro_voice);
                kokoro_voice_picker_open_ = true;
                feedback.play(audio::Cue::modal_open);
            }
            else
            {
                selecting_model_ = false;
                const int session_index = sessions_.focus() - workspace_sessions_row_;
                if (session_index >= 0 &&
                    static_cast<std::size_t>(session_index) < visible_sessions_.size() &&
                    app_.open_session(visible_sessions_[static_cast<std::size_t>(session_index)]))
                {
                    rail_ = false;
                    composer_.clear();
                    page_age_ = 0;
                    conversation_restore_pending_ = true;
                }
                else
                    feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.7f);
            }
        }
    }
    else if (input.is_pressed(Action::back) && app_.state().messages.empty() && !app_.busy())
    {
        // An empty conversation steps back to the opening page.
        conversation_ = false;
        page_age_ = 0;
        feedback.play(audio::Cue::back);
    }
    else if (input.is_pressed(Action::confirm))
        submit(feedback);
    else if (input.is_pressed(Action::north))
    {
        bool accepted = false;
        if (app_.state().retry_available)
            accepted = app_.retry();
        else
        {
            const int selected = selected_audio_row();
            accepted =
                selected >= 0
                    ? app_.play_audio(chat_rows_[static_cast<std::size_t>(selected)].audio_path)
                    : app_.play_audio();
        }
        feedback.play(accepted ? audio::Cue::select : audio::Cue::error, 1.0f, 0.0f,
                      accepted ? 1.0f : 0.7f);
    }
    else if (input.nav == Direction::up)
        chat_.scroll_by(0, -140);
    else if (input.nav == Direction::down)
        chat_.scroll_by(0, 140);
}

void NativeUI::Impl::open_dialog(int action, int index, std::string title, std::string body,
                                 const char *button, bool destructive, ui::Feedback &feedback,
                                 const char *other)
{
    dialog_action_ = action;
    dialog_index_ = index;
    ui::DialogContent content;
    content.title = std::move(title);
    content.body = std::move(body);
    content.buttons = {{other, ui::ButtonKind::secondary, false},
                       {button, ui::ButtonKind::primary, destructive}};
    dialog_.open(std::move(content), feedback);
}

void NativeUI::Impl::refresh_models()
{
    visible_models_.clear();
    std::string query = search_.text();
    const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::transform(query.begin(), query.end(), query.begin(), lower);
    const auto &catalog = app_.state().models;
    for (std::size_t i = 0; i < catalog.size(); ++i)
    {
        const int capability = static_cast<int>(catalog[i].capability) + 1;
        std::string name = catalog[i].name + " " + catalog[i].id;
        std::transform(name.begin(), name.end(), name.begin(), lower);
        const int selected_filter = filters_.active() == 0 ? 0 : filters_.active() - 1;
        if ((selected_filter == 0 || selected_filter == capability) &&
            name.find(query) != std::string::npos)
            visible_models_.push_back(static_cast<int>(i));
    }
    for (std::size_t i = 0; i < prospero_model_download::preset_count; ++i)
    {
        const auto &preset = prospero_model_download::presets[i];
        preset_saved_[i] = prospero_model_download::preset_installed(i);
        // Installed text models already have a runtime card; media bundles stay visible
        // in the Vulkan library so their runtime requirement remains clear.
        if (preset.kind == 0 && preset_saved_[i])
            continue;
        if (std::any_of(catalog.begin(), catalog.end(),
                        [&](const Model &model) {
                            return model.id == preset.id ||
                                   (preset.kind == 0 && model.id == preset.files[0].source);
                        }))
            continue;
        std::string name = std::string(preset.name) + " " + preset.id;
        std::transform(name.begin(), name.end(), name.begin(), lower);
        const int selected_filter = filters_.active() == 0 ? 0 : filters_.active() - 1;
        if ((selected_filter == 0 || selected_filter == preset.kind + 1) &&
            name.find(query) != std::string::npos)
            visible_models_.push_back(-static_cast<int>(i) - 2);
    }
    models_.set_count(static_cast<int>(visible_models_.size()));
    models_.set_focus(0);
    models_.enter();
}

void NativeUI::Impl::refresh_sessions()
{
    static constexpr const char *kinds[] = {"all", "text", "image", "audio", "voice"};
    const auto &state = app_.state();
    std::vector<ui::ListItem> rows;
    if (selecting_model_)
    {
        workspace_model_indices_.clear();
        std::string query = workspace_model_query_;
        std::transform(query.begin(), query.end(), query.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (unsigned i = 0; i < state.models.size(); ++i)
        {
            const auto &model = state.models[i];
            if (workspace_model_filter_ >= 0 &&
                static_cast<int>(model.capability) != workspace_model_filter_)
                continue;
            std::string searchable = model.name + " " + model.id + " " + model.purpose;
            std::transform(searchable.begin(), searchable.end(), searchable.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!query.empty() && searchable.find(query) == std::string::npos)
                continue;
            ui::ListItem item;
            item.title = model.name;
            static constexpr const char *kCapabilityNames[] = {"Text", "Image", "Audio", "Voice"};
            const bool selected = static_cast<int>(i) == state.selected_model;
            item.subtitle = std::string(kCapabilityNames[static_cast<unsigned>(model.capability)]) +
                            (selected ? (state.ready       ? " · Ready"
                                         : state.preparing ? " · Loading…"
                                                           : " · Selected, not prepared")
                                      : " · Select to load");
            rows.push_back(std::move(item));
            workspace_model_indices_.push_back(i);
        }
        if (rows.empty())
        {
            ui::ListItem item;
            item.title = state.models.empty() ? "No models installed" : "No matching models";
            item.subtitle = state.models.empty() ? "Install a model before starting a conversation"
                                                 : "Change or clear your model search";
            rows.push_back(std::move(item));
        }
        bool changed = rows.size() != sessions_.items().size();
        for (std::size_t i = 0; !changed && i < rows.size(); ++i)
            changed = rows[i].title != sessions_.items()[i].title ||
                      rows[i].subtitle != sessions_.items()[i].subtitle;
        if (changed)
        {
            const int focus = sessions_.focus();
            sessions_.set_items(std::move(rows));
            sessions_.set_focus(
                std::min<int>(focus, static_cast<int>(sessions_.items().size()) - 1));
        }
        return;
    }
    workspace_model_indices_.clear();
    workspace_voice_row_ = -1;
    workspace_language_row_ = -1;
    workspace_new_conversation_row_ = -1;
    ui::ListItem first;
    char loaded_id[256]{}, loaded_name[256]{};
    const bool model_loaded =
        gpt_runtime_loaded_model(loaded_id, sizeof(loaded_id), loaded_name, sizeof(loaded_name));
    first.title = model_loaded ? (loaded_name[0] ? loaded_name : loaded_id) : "Choose model";
    first.subtitle = model_loaded ? "Loaded in memory · Change model"
                     : state.selected_model >= 0
                         ? std::string(state.ready ? "Loaded in memory · " : "Selected · ") +
                               state.models[static_cast<std::size_t>(state.selected_model)].name
                         : "Select one to load it into memory";
    rows.push_back(std::move(first));
    ui::ListItem second;
    second.title = "Generation settings";
    second.subtitle = "Response · context · thinking";
    rows.push_back(std::move(second));
    if (state.selected_model >= 0 &&
        state.models[static_cast<std::size_t>(state.selected_model)].capability ==
            Capability::Voice)
    {
        workspace_voice_row_ = static_cast<int>(rows.size());
        ui::ListItem voice;
        const bool chatterbox =
            state.models[static_cast<std::size_t>(state.selected_model)].id.find("chatterbox") !=
            std::string::npos;
        voice.title = chatterbox ? "Chatterbox voice" : "Kokoro voice";
        const auto voice_list = prospero_voice::runtime_voices(chatterbox);
        const auto selected_index =
            chatterbox ? state.preferences.chatterbox_voice : state.preferences.kokoro_voice;
        const auto &selected_voice =
            voice_list[std::min<std::size_t>(selected_index, voice_list.size() - 1)];
        voice.subtitle =
            selected_voice.name + " · " + selected_voice.locale + " · Select to change";
        rows.push_back(std::move(voice));
        if (chatterbox)
        {
            // The voice is the timbre; this is the language the model speaks it in.
            workspace_language_row_ = static_cast<int>(rows.size());
            ui::ListItem language;
            language.title = "Chatterbox language";
            const auto &spoken =
                prospero_language::chatterbox_language(state.preferences.chatterbox_language);
            language.subtitle = std::string(spoken.name) + " · Select to change";
            rows.push_back(std::move(language));
        }
    }
    workspace_new_conversation_row_ = static_cast<int>(rows.size());
    ui::ListItem new_conversation;
    new_conversation.title = "New conversation";
    new_conversation.subtitle = "Start fresh";
    rows.push_back(std::move(new_conversation));
    workspace_sessions_row_ = static_cast<int>(rows.size());
    visible_sessions_.clear();
    for (unsigned i = 0; i < state.sessions.size(); ++i)
    {
        const auto &session = state.sessions[i];
        if (!prospero_session::matches(session, kinds[session_category_], ""))
            continue;
        bool found = prospero_session::matches(session, "all", session_query_.c_str());
        if (!found && i < state.session_search.size() && state.session_search[i].text)
            found = prospero_session::contains(state.session_search[i].text->c_str(),
                                               session_query_.c_str());
        if (!found)
            continue;
        visible_sessions_.push_back(i);
        ui::ListItem item;
        item.title = session.title;
        item.subtitle =
            std::string(prospero_session::category(session)) + " · " + session.model_name;
        font_set_.note(item.title);
        rows.push_back(std::move(item));
    }
    bool changed = rows.size() != sessions_.items().size();
    for (std::size_t i = 0; !changed && i < rows.size(); ++i)
        changed = rows[i].title != sessions_.items()[i].title ||
                  rows[i].subtitle != sessions_.items()[i].subtitle;
    if (changed)
    {
        const int focus = sessions_.focus();
        sessions_.set_items(std::move(rows));
        sessions_.set_focus(focus);
    }
}

void NativeUI::Impl::begin_model_selection()
{
    selecting_model_ = true;
    workspace_model_query_.clear();
    workspace_model_filter_ = -1;
    conversation_ = true;
    rail_ = true;
    sessions_.set_focus(0);
    refresh_sessions();
    page_age_ = 0;
}

void NativeUI::Impl::type(char character, ui::Feedback &feedback)
{
    if (dialog_.is_open() || update_ui_ != UpdateUi::hidden || keyboard_pending_ || boot_ > 0.6f)
        return;
    if (tabs_.active() == 0 && search_open_)
    {
        if (selecting_model_)
        {
            if (workspace_model_query_.size() < 96)
                workspace_model_query_.push_back(character);
        }
        else
        {
            session_search_.insert(character, feedback);
            session_query_ = session_search_.text();
        }
        refresh_sessions();
    }
    else if (tabs_.active() == 1)
    {
        search_.insert(character, feedback);
        refresh_models();
    }
    else if (tabs_.active() == 0 && !app_.busy())
    {
        if (!conversation_)
            page_age_ = 0;
        conversation_ = true;
        rail_ = false;
        composer_.insert(character, feedback);
    }
}

void NativeUI::Impl::backspace(ui::Feedback &feedback)
{
    if (dialog_.is_open() || update_ui_ != UpdateUi::hidden || keyboard_pending_ || boot_ > 0.6f)
        return;
    if (tabs_.active() == 0 && search_open_)
    {
        if (selecting_model_)
        {
            if (!workspace_model_query_.empty())
                workspace_model_query_.pop_back();
        }
        else
        {
            session_search_.backspace(feedback);
            session_query_ = session_search_.text();
        }
        refresh_sessions();
    }
    else if (tabs_.active() == 1)
    {
        search_.backspace(feedback);
        refresh_models();
    }
    else if (tabs_.active() == 0 && !app_.busy())
        composer_.backspace(feedback);
}

void NativeUI::Impl::submit(ui::Feedback &feedback)
{
    if (search_open_)
    {
        search_open_ = false;
        search_.set_active(false);
        feedback.play(audio::Cue::modal_close);
        return;
    }
    if (tabs_.active() != 0 || dialog_.is_open() || update_ui_ != UpdateUi::hidden ||
        keyboard_pending_ || boot_ > 0.6f)
        return;
    if (app_.busy())
    {
        feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        return;
    }
    if (!app_.can_send())
    {
        begin_model_selection();
        feedback.play(audio::Cue::open);
        return;
    }
    if (!conversation_)
    {
        begin_model_selection();
        feedback.play(audio::Cue::open);
        return;
    }
    conversation_ = true;
    rail_ = false;
    if (!composer_.text().empty())
    {
        if (app_.send(composer_.text()))
        {
            composer_.clear();
            send_pulse_.trigger();
            feedback.play(audio::Cue::select);
            chat_.scroll_to(0, chat_.max_y());
        }
        else
            feedback.play(audio::Cue::error);
    }
    else
    {
        chat_keyboard_open_ = true;
        keyboard_.style.max_length = 1023;
        keyboard_.set_length(composer_.text().length());
        keyboard_.enter();
        feedback.play(audio::Cue::open);
    }
}

void NativeUI::Impl::keyboard_result(const char *value)
{
    keyboard_pending_ = false;
    if (value && *value)
    {
        composer_.set_text(value);
        // The OS keyboard's Done action sends, matching the original app.
        if (app_.send(composer_.text()))
        {
            composer_.clear();
            send_pulse_.trigger();
        }
    }
}

void NativeUI::Impl::build_form()
{
    auto p = app_.state().preferences;
    const auto runtime = prospero_settings::get();
    p.context_length = runtime.context;
    p.output_limit = runtime.output_limit;
    p.kv_cache = runtime.kv;
    p.thinking = runtime.thinking;
    p.idle_unload_minutes = runtime.idle_unload_minutes;
    p.kokoro_voice = runtime.kokoro_voice;
    form_.clear();
    form_.set_bounds({494, 362, 912, 520});
    if (category_ == 0)
    {
        form_.add_choice(1, "Color theme", {"Midnight", "Daylight"}, static_cast<int>(p.theme));
        form_.add_choice(2, "Accent color", {"Amber", "Mist", "Sage"}, static_cast<int>(p.accent));
    }
    else if (category_ == 1)
        form_.add_slider(3, "Interface sounds", static_cast<float>(p.volume), 0, 100, 5).unit = "%";
    else if (category_ == 2)
    {
        form_.add_toggle(4, "Reduce motion", p.reduced_motion);
        form_.add_toggle(5, "High contrast", p.high_contrast);
        form_.add_choice(8, "Reading size", {"Standard", "Large"}, static_cast<int>(p.text_size));
    }
    else if (category_ == kDiagnostics)
    {
        // Technical wording, as the trace itself is.
        form_.add_toggle(15, "Debug log", debug::enabled()).description =
            "Timed trace of everything the app does, for reporting a problem. Off by default.";
        form_.add_value(16, "Logs folder", logs_folder_text());
    }
    else
    {
        const std::string label = PROSPERO_BUILD_LABEL;
        form_.add_value(10, "Version",
                        label.empty() ? PROSPERO_VERSION : PROSPERO_VERSION "  \xC2\xB7  " + label);
        form_.add_value(11, "Made by", "BlackBearReloaded and Quatrixone");
        form_.add_value(12, "Interface", "ps5-homebrew-ui");
        form_.add_value(13, "Graphics", "ps5-opengl");
        form_.add_value(14, "License", "GPL-3.0-or-later");
    }
    form_.enter();
}

void NativeUI::Impl::build_generation_form()
{
    const auto p = app_.state().preferences;
    const auto runtime = prospero_settings::get();
    const auto &state = app_.state();
    const Capability capability =
        state.selected_model >= 0
            ? state.models[static_cast<std::size_t>(state.selected_model)].capability
            : Capability::Text;
    form_.clear();
    form_.set_bounds({536, 262, 1288, 676});
    if (capability == Capability::Text)
    {
        form_.add_header("Text responses");
        auto &style_field = form_.add_choice(
            6, "Response style", {"Balanced", "Precise", "Creative"}, static_cast<int>(p.style));
        style_field.description =
            "Choose how concise, factual or imaginative text replies should be.";
        const unsigned output = runtime.output_limit;
        auto &length_field = form_.add_choice(
            7, "Response length",
            {"Auto", "64 tokens", "128 tokens", "192 tokens", "256 tokens", "512 tokens",
             "1024 tokens", "2048 tokens", "4096 tokens", "8192 tokens", "16384 tokens",
             "32768 tokens", "65536 tokens", "131072 tokens"},
            output == 0       ? 0
            : output <= 256   ? static_cast<int>(output / 64)
            : output == 512   ? 5
            : output == 1024  ? 6
            : output == 2048  ? 7
            : output == 4096  ? 8
            : output == 8192  ? 9
            : output == 16384 ? 10
            : output == 32768 ? 11
                              : 12);
        length_field.description =
            "Auto uses the available context. Larger limits can take longer and use more memory.";
        form_.add_header("Advanced text settings");
        constexpr unsigned contexts[] = {0, 4096, 8192, 16384, 32768, 65536, 131072, 262144};
        int selected_context = 0;
        for (int i = 0; i < 8; ++i)
            if (contexts[i] == runtime.context)
                selected_context = i;
        auto &context_field = form_.add_choice(
            17, "Context length", {"Auto", "4K", "8K", "16K", "32K", "64K", "128K", "256K"},
            selected_context);
        context_field.description = "How much of the conversation the text model can consider. "
                                    "Larger contexts need more memory.";
        auto &thinking_field = form_.add_toggle(19, "Thinking", runtime.thinking);
        thinking_field.description =
            "Allow supported text models to spend extra tokens reasoning before answering.";
    }
    form_.add_header("Model memory");
    form_.add_value(21, "Memory use", "Unload the selected model after it has been idle.");
    constexpr unsigned idle_minutes[] = {1, 2, 5, 10, 30, 60};
    int selected_idle = 1;
    for (int i = 0; i < 6; ++i)
        if (idle_minutes[i] == runtime.idle_unload_minutes)
            selected_idle = i;
    auto &unload_field = form_.add_choice(
        20, "Unload model after",
        {"1 minute", "2 minutes", "5 minutes", "10 minutes", "30 minutes", "60 minutes"},
        selected_idle);
    unload_field.description =
        "Free model memory after this idle time. The next request will load the model again.";
    const bool audio_model =
        state.selected_model >= 0 &&
        state.models[static_cast<std::size_t>(state.selected_model)].capability ==
            Capability::Audio;
    if (audio_model)
    {
        form_.add_header("Audio generation");
        constexpr unsigned durations[] = {0, 8, 16, 30, 60, 120, 300};
        int selected_duration = 0;
        for (int i = 0; i < 7; ++i)
            if (durations[i] == runtime.audio_duration_sec)
                selected_duration = i + 1;
        form_.add_value(29, "Current audio duration",
                        runtime.audio_duration_sec == 0
                            ? "Model default"
                            : std::to_string(runtime.audio_duration_sec) + " seconds");
        auto &duration_field =
            form_.add_choice(24, "Audio clip duration",
                             {"Keep current", "Model default", "8 seconds", "16 seconds",
                              "30 seconds", "60 seconds", "120 seconds", "300 seconds"},
                             selected_duration);
        duration_field.description = "Choose the clip length. Model default lets the model choose; "
                                     "longer clips take more time.";
        form_.add_value(27, "Current audio seed", std::to_string(runtime.audio_seed));
        auto &audio_seed_field = form_.add_choice(
            28, "Audio seed", {"Keep current", "42", "123", "1234", "2026", "0"}, 0);
        audio_seed_field.description = "The same seed and settings can reproduce a result when the "
                                       "model supports seeded output.";
    }
    const bool image_model =
        state.selected_model >= 0 &&
        state.models[static_cast<std::size_t>(state.selected_model)].capability ==
            Capability::Image;
    if (image_model)
    {
        form_.add_header("Image generation");
        form_.add_value(25, "Current image seed",
                        runtime.sd_seed == 0 ? "Random" : std::to_string(runtime.sd_seed));
        auto &image_seed_field = form_.add_choice(
            26, "Image seed", {"Keep current", "42", "123", "1234", "2026", "Random"}, 0);
        image_seed_field.description =
            "Use the same seed to make similar results, or Random to vary each image.";
        std::string image_size = "Model default";
        if (runtime.sd_width && runtime.sd_height)
            image_size =
                std::to_string(runtime.sd_width) + " x " + std::to_string(runtime.sd_height);
        form_.add_value(30, "Current image size", image_size);
        auto &image_size_field = form_.add_choice(
            31, "Image size",
            {"Keep current", "Model default", "512 x 512", "768 x 768", "512 x 768", "768 x 512"},
            0);
        image_size_field.description = "Larger images need more memory and take longer. "
                                       "Unsupported sizes use the model default.";
        auto &upscale_field = form_.add_choice(32, "Image upscaler", {"Off", "Real-ESRGAN 4x"},
                                               runtime.sd_upscale ? 1 : 0);
        upscale_field.description =
            "Optional 4x enlargement. It needs the upscaler weights and additional memory.";
        if (runtime.sd_upscale)
            form_.add_value(33, "Upscaler model path", "models/upscalers/RealESRGAN_x4plus.pth");
    }
    form_.add_header("Online tools");
    form_.add_value(9, "Processing", "On this console");
    auto &search_field =
        form_.add_choice(22, "Web search", {"Off", "LibreY"}, runtime.web_search ? 1 : 0);
    search_field.description = "When on, supported text models can search the web. Search terms "
                               "are sent to the configured providers.";
    form_.add_value(23, "Search engine", "Built into ProsperoAI");
    form_.enter();
}

void NativeUI::Impl::apply_form()
{
    auto p = app_.state().preferences;
    if (category_ == 0)
    {
        p.theme = static_cast<unsigned>(form_.choice_index(1));
        p.accent = static_cast<unsigned>(form_.choice_index(2));
    }
    else if (category_ == 1)
        p.volume = static_cast<unsigned>(form_.slider_value(3));
    else if (category_ == 2)
    {
        p.reduced_motion = form_.toggle_value(4);
        p.high_contrast = form_.toggle_value(5);
        p.text_size = static_cast<unsigned>(form_.choice_index(8));
    }
    else if (category_ == kDiagnostics)
    {
        // Not a preference: the switch is a file, so that it holds from the next start's
        // first moment.
        const bool wanted = form_.toggle_value(15);
        if (wanted != debug::enabled())
        {
            if (!debug::set_enabled(wanted))
                toasts_.push(ui::StatusKind::danger, "Could not switch the debug log",
                             "Try again.");
            else if (wanted)
                toasts_.push(ui::StatusKind::info, "Debug log is on",
                             "Repeat the problem, then send debug-trace.txt from the logs folder.",
                             6);
            form_.set_toggle(15, debug::enabled());
        }
        return;
    }
    debug::line("setting", "category %d changed", category_);
    app_.set_preferences(std::move(p));
    apply_theme();
}

void NativeUI::Impl::apply_generation_form()
{
    auto p = app_.state().preferences;
    const auto &state = app_.state();
    const Capability capability =
        state.selected_model >= 0
            ? state.models[static_cast<std::size_t>(state.selected_model)].capability
            : Capability::Text;
    if (capability == Capability::Text)
    {
        p.style = static_cast<unsigned>(form_.choice_index(6));
        constexpr unsigned limits[] = {0,    64,   128,  192,   256,   512,   1024,
                                       2048, 4096, 8192, 16384, 32768, 65536, 131072};
        p.output_limit = limits[std::clamp(form_.choice_index(7), 0, 13)];
        constexpr unsigned contexts[] = {0, 4096, 8192, 16384, 32768, 65536, 131072, 262144};
        p.context_length = contexts[std::clamp(form_.choice_index(17), 0, 7)];
        p.kv_cache = 0;
        p.thinking = form_.toggle_value(19);
    }
    constexpr unsigned idle_minutes[] = {1, 2, 5, 10, 30, 60};
    p.idle_unload_minutes = idle_minutes[std::clamp(form_.choice_index(20), 0, 5)];
    app_.set_preferences(std::move(p));
    auto runtime = prospero_settings::get();
    const bool audio_model =
        state.selected_model >= 0 &&
        state.models[static_cast<std::size_t>(state.selected_model)].capability ==
            Capability::Audio;
    const bool image_model =
        state.selected_model >= 0 &&
        state.models[static_cast<std::size_t>(state.selected_model)].capability ==
            Capability::Image;
    bool runtime_changed = false;
    if (audio_model)
    {
        constexpr unsigned durations[] = {0, 0, 8, 16, 30, 60, 120, 300};
        const int selected_duration = std::clamp(form_.choice_index(24), 0, 7);
        if (selected_duration != 0)
            runtime.audio_duration_sec = durations[selected_duration];
        constexpr unsigned seeds[] = {0, 42, 123, 1234, 2026, 0};
        const int selected_seed = std::clamp(form_.choice_index(28), 0, 5);
        if (selected_seed != 0)
            runtime.audio_seed = seeds[selected_seed];
        runtime_changed = runtime_changed || selected_duration != 0 || selected_seed != 0;
    }
    if (image_model)
    {
        constexpr unsigned seeds[] = {0, 42, 123, 1234, 2026, 0};
        const int selected_seed = std::clamp(form_.choice_index(26), 0, 5);
        if (selected_seed != 0)
            runtime.sd_seed = seeds[selected_seed];
        constexpr unsigned widths[] = {0, 0, 512, 768, 512, 768};
        constexpr unsigned heights[] = {0, 0, 512, 768, 768, 512};
        const int selected_size = std::clamp(form_.choice_index(31), 0, 5);
        if (selected_size != 0)
        {
            runtime.sd_width = widths[selected_size];
            runtime.sd_height = heights[selected_size];
        }
        runtime_changed = runtime_changed || selected_seed != 0 || selected_size != 0;
        runtime.sd_upscale = form_.choice_index(32) == 1;
        runtime_changed =
            runtime_changed || runtime.sd_upscale != prospero_settings::get().sd_upscale;
    }
    runtime.web_search = form_.choice_index(22) == 1;
    const bool search_changed = runtime.web_search != prospero_settings::get().web_search;
    if ((runtime_changed || search_changed) && !prospero_settings::set(runtime))
    {
        toasts_.push(ui::StatusKind::warning, "Generation settings not saved",
                     "Could not save generation settings.");
        build_generation_form();
    }
}

void NativeUI::Impl::sync()
{
    const auto &state = app_.state();
    revision_ = state.revision;
    const bool faces_changed = font_revision_ != font_set_.revision();
    font_revision_ = font_set_.revision();
    for (auto it = images_.begin(); it != images_.end();)
    {
        const bool retained =
            std::any_of(state.images.begin(), state.images.end(),
                        [&](const auto &image) { return image->path == it->first; });
        if (retained)
            ++it;
        else
        {
            glDeleteTextures(1, &it->second.id);
            it = images_.erase(it);
        }
    }
    active_model_ = state.selected_model;
    if (active_model_ >= 0)
    {
        const char *prompts[] = {
            "Take an idea a little further...", "Describe an image you want to create...",
            "Describe a sound or atmosphere...", "Write the words you would like to hear..."};
        composer_.set_placeholder(prompts[static_cast<unsigned>(
            state.models[static_cast<std::size_t>(active_model_)].capability)]);
    }
    apply_theme();
    if (catalog_count_ != static_cast<int>(state.models.size()))
    {
        catalog_count_ = static_cast<int>(state.models.size());
        for (const auto &model : state.models)
            font_set_.note(model.name);
        refresh_models();
        build_form();
    }
    refresh_sessions();
    font_set_.note(composer_.text());

    const bool follow = conversation_restore_pending_ || chat_.max_y() - chat_.offset_y() < 80;
    const float size = state.preferences.text_size ? 34.0f : 28.0f;
    const bool remeasure = body_size_ != size || faces_changed;
    body_size_ = size;
    const float line = body_size_ * 1.48f;
    // While an answer is on its way it has a row of its own, empty until the first words.
    const bool streaming = app_.generating();
    const std::size_t previous = chat_rows_.size();
    chat_rows_.resize(state.messages.size() + (streaming ? 1 : 0));
    float y = 0;
    for (std::size_t i = 0; i < chat_rows_.size(); ++i)
    {
        auto &row = chat_rows_[i];
        const bool stored = i < state.messages.size();
        const char *value = stored ? state.messages[i].content.c_str() : state.stream.c_str();
        const char *role = stored ? state.messages[i].role : "assistant";
        const bool user = std::strcmp(role, "user") == 0;
        std::string visible = value;
        std::string image_path;
        std::string audio_path;
        bool audio = false;
        if (std::strcmp(role, "assistant") == 0)
        {
            const auto extract_path = [&](const char *extension)
            {
                constexpr char prefix[] = "/download0/";
                const std::size_t extension_length = std::strlen(extension);
                for (auto start = visible.find(prefix); start != std::string::npos;
                     start = visible.find(prefix, start + sizeof(prefix) - 1))
                {
                    const auto end = visible.find(extension, start);
                    const auto newline = visible.find_first_of("\r\n", start);
                    if (end != std::string::npos && (newline == std::string::npos || newline > end))
                        return visible.substr(start, end + extension_length - start);
                }
                return std::string{};
            };
            image_path = extract_path(".tga");
            audio_path = extract_path(".wav");
            audio = !audio_path.empty();
            const auto media_start = visible.find("/download0/");
            if ((!image_path.empty() || audio) && media_start != std::string::npos)
                visible.resize(media_start);
            while (!visible.empty() && (visible.back() == '\n' || visible.back() == '\r'))
                visible.pop_back();
        }
        if (i >= previous)
            row.age = 0;
        if (remeasure || row.text != value || row.role != role || row.image_path != image_path ||
            row.audio_path != audio_path)
        {
            row.text = value;
            row.role = role;
            row.image_path = std::move(image_path);
            row.audio_path = std::move(audio_path);
            row.audio = audio;
            font_set_.note(visible);
            row.lines = fonts_.regular.font->wrap(visible, body_size_, user ? 900.0f : 1240.0f);
            row.width = 0;
            for (const auto &text_line : row.lines)
                row.width = std::max(row.width, fonts_.regular.measure(text_line, body_size_));
            const float lines = static_cast<float>(std::max<std::size_t>(row.lines.size(), 1));
            row.height = user ? lines * line + 34 + 28 : 58 + lines * line + 30;
            if (!row.image_path.empty())
                row.height += 382;
            else if (row.audio)
                row.height += 112;
        }
        row.y = y;
        y += row.height;
    }
    chat_.set_content_size(chat_.bounds().w, y);
    if (follow)
        chat_.scroll_to(0, chat_.max_y());
    conversation_restore_pending_ = false;
    if (!state.messages.empty() && !conversation_)
    {
        conversation_ = true;
        page_age_ = 0;
    }
}

void NativeUI::Impl::upload_visible_image()
{
    if (tabs_.active() != 0 || !conversation_)
        return;
    // Keep at most four app-owned image textures resident. Conversation images are
    // retained as decoded data by the app; GPU copies outside the viewport are LRU.
    for (const auto &row : chat_rows_)
    {
        if (row.image_path.empty() || row.y + row.height < chat_.offset_y() ||
            row.y > chat_.offset_y() + chat_.bounds().h)
            continue;
        const auto found = images_.find(row.image_path);
        if (found != images_.end())
            found->second.last_used = clock_;
    }
    for (const auto &row : chat_rows_)
    {
        if (row.image_path.empty() || images_.count(row.image_path) ||
            row.y + row.height < chat_.offset_y() || row.y > chat_.offset_y() + chat_.bounds().h)
            continue;
        for (const auto &image : app_.state().images)
        {
            if (image->path != row.image_path)
                continue;
            const auto texture = renderer_.batch().create_texture(static_cast<int>(image->width),
                                                                  static_cast<int>(image->height),
                                                                  image->rgba.data());
            if (texture)
            {
                constexpr std::size_t kMaxResidentImageTextures = 4;
                if (images_.size() >= kMaxResidentImageTextures)
                {
                    auto oldest = images_.end();
                    for (auto candidate = images_.begin(); candidate != images_.end(); ++candidate)
                    {
                        const bool visible = std::any_of(
                            chat_rows_.begin(), chat_rows_.end(),
                            [&](const ChatRow &visible_row)
                            {
                                return visible_row.image_path == candidate->first &&
                                       visible_row.y + visible_row.height >= chat_.offset_y() &&
                                       visible_row.y <= chat_.offset_y() + chat_.bounds().h;
                            });
                        if (!visible && (oldest == images_.end() ||
                                         candidate->second.last_used < oldest->second.last_used))
                            oldest = candidate;
                    }
                    // If more than four previews are visible, leave the extra preview
                    // deferred until scrolling frees a texture; avoid per-frame churn.
                    if (oldest == images_.end())
                        return;
                    glDeleteTextures(1, &oldest->second.id);
                    images_.erase(oldest);
                }
                images_.emplace(image->path, Texture{texture,
                                                     static_cast<float>(image->width) /
                                                         static_cast<float>(image->height),
                                                     clock_});
            }
            return; // At most one bounded upload per frame.
        }
    }
}

void NativeUI::Impl::release()
{
    voice_recorder_.discard();
    for (const auto &[path, texture] : images_)
        glDeleteTextures(1, &texture.id);
    images_.clear();
}

void NativeUI::Impl::draw(UiFrame &frame) const
{
    const auto &preferences = app_.state().preferences;
    if (preferences.high_contrast)
    {
        // Nothing moves or glows behind the text.
        frame.backdrop.mode = gfx::BackdropMode::gradient;
        frame.backdrop.colors[0] = palette_.page_top;
        frame.backdrop.colors[1] = palette_.page_bottom;
    }
    else if (palette_.day)
    {
        // Daylight: paper under a slow light. The clouds' darkened corners would
        // only grey a pale page.
        frame.backdrop.mode = gfx::BackdropMode::gradient;
        frame.backdrop.colors[0] = page_top_.value();
        frame.backdrop.colors[1] = page_bottom_.value();
        frame.backdrop.colors[2] =
            gfx::mix(cloud_a_.value(), cloud_b_.value(), 0.5f + 0.5f * std::sin(clock_ * 0.11f));
        frame.backdrop.params[0] = 0.72f + 0.10f * std::sin(clock_ * 0.07f);
        frame.backdrop.params[1] = 0.30f + 0.08f * std::cos(clock_ * 0.05f);
        frame.backdrop.params[2] = 0.16f;
    }
    else
    {
        frame.backdrop.mode = gfx::BackdropMode::aurora;
        frame.backdrop.colors[0] = page_top_.value();
        frame.backdrop.colors[1] = page_bottom_.value();
        frame.backdrop.colors[2] = cloud_a_.value();
        frame.backdrop.colors[3] = cloud_b_.value();
        frame.backdrop.time = 40 + clock_;
    }
    auto &list = frame.scene;
    ui::Canvas canvas{list, fonts_, 0, clock_};
    draw_header(canvas);
    const float entrance = arrival(0);
    list.push_transform(1, 0, 0, 0, (1 - entrance) * 18);
    list.push_opacity(entrance);
    if (tabs_.active() == 1)
        draw_models(canvas);
    else if (tabs_.active() == 2)
        draw_settings(canvas);
    else if (tabs_.active() == 3)
        draw_mobile(canvas);
    else if (conversation_)
        draw_conversation(canvas);
    else
        draw_welcome(canvas);
    list.pop_opacity();
    list.pop_transform();
    draw_footer(list);
    if (boot_ > 0.001f)
        draw_boot(list);

    ui::Canvas overlay{frame.overlay, fonts_, 0, clock_};
    if (search_open_ || chat_keyboard_open_)
        draw_search(overlay);
    if (dialog_.visible())
    {
        frame.glass = true;
        ui::Canvas frosted{frame.overlay, fonts_, frame.glass_texture, clock_};
        dialog_.draw(frosted);
    }
    if (kokoro_voice_picker_open_)
    {
        frame.glass = true;
        auto &popup = frame.overlay;
        popup.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                           Color::rgb(0x000000, palette_.day ? 0.45f : 0.72f));
        const Rect panel{490, 220, 940, 640};
        popup.shadow({panel.x, panel.y + 14, panel.w, panel.h}, 24, 50, Color::rgb(0x000000, 0.4f));
        popup.bordered_rect(panel, 24, palette_.panel.with_alpha(1.0f / palette_.panel.a), 1,
                            palette_.line);
        label(popup, voice_picker_chatterbox_ ? "CHATTERBOX VOICE" : "KOKORO VOICE", panel.x + 42,
              panel.y + 52, palette_.accent);
        text(popup, "Choose a voice", panel.x + 42, panel.y + 101, 34, palette_.ink, true);
        const auto voice_list = prospero_voice::runtime_voices(voice_picker_chatterbox_);
        const int voice_count = static_cast<int>(voice_list.size());
        const int first_voice =
            std::min(std::max(0, kokoro_voice_picker_focus_ - 2), std::max(0, voice_count - 5));
        for (int i = first_voice; i < std::min(voice_count, first_voice + 5); ++i)
        {
            const float y = panel.y + 140 + static_cast<float>(i - first_voice) * 70;
            const Rect row{panel.x + 28, y, panel.w - 56, 60};
            if (i == kokoro_voice_picker_focus_)
                popup.rounded_rect(row, 12, palette_.accent.with_alpha(0.22f));
            const auto &voice = voice_list[static_cast<std::size_t>(i)];
            text(popup, voice.name, row.x + 20, row.y + 22, 24,
                 i == kokoro_voice_picker_focus_ ? palette_.ink : palette_.muted,
                 i == kokoro_voice_picker_focus_);
            text(popup, voice.locale, row.x + row.w - 20, row.y + 22, 21, palette_.muted, false,
                 gfx::Align::right);
        }
        popup.line(panel.x + 28, panel.y + panel.h - 88, panel.x + panel.w - 28,
                   panel.y + panel.h - 88, 1, palette_.line);
        ui::Hint hints[5]{{ui::Button::dpad, "Select"},
                          {ui::Button::cross, "Use voice"},
                          {ui::Button::triangle, "Record"},
                          {ui::Button::left_stick, "L3 Delete"},
                          {ui::Button::circle, "Back"}};
        if (!voice_picker_chatterbox_)
            hints[2] = hints[4];
        const int count = voice_picker_chatterbox_ ? 5 : 3;
        auto glyphs = palette_.day ? ui::GlyphStyle::light() : ui::GlyphStyle::dark();
        glyphs.label = palette_.ink;
        ui::HintLayout layout;
        layout.size = 30;
        layout.text_size = 21;
        layout.cy = panel.y + panel.h - 43;
        layout.item_gap = 26;
        const float width = ui::measure_hints(fonts_, hints, count, layout);
        ui::draw_hints(popup, fonts_, glyphs, hints, count, panel.cx() - width / 2, false, layout);
    }
    if (language_picker_open_)
    {
        frame.glass = true;
        auto &popup = frame.overlay;
        popup.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                           Color::rgb(0x000000, palette_.day ? 0.45f : 0.72f));
        const Rect panel{490, 220, 940, 640};
        popup.shadow({panel.x, panel.y + 14, panel.w, panel.h}, 24, 50, Color::rgb(0x000000, 0.4f));
        popup.bordered_rect(panel, 24, palette_.panel.with_alpha(1.0f / palette_.panel.a), 1,
                            palette_.line);
        label(popup, "CHATTERBOX LANGUAGE", panel.x + 42, panel.y + 52, palette_.accent);
        text(popup, "Choose a language", panel.x + 42, panel.y + 101, 34, palette_.ink, true);
        const int language_count = static_cast<int>(prospero_language::chatterbox_count);
        const int first_language =
            std::min(std::max(0, language_picker_focus_ - 2), std::max(0, language_count - 5));
        for (int i = first_language; i < std::min(language_count, first_language + 5); ++i)
        {
            const float y = panel.y + 140 + static_cast<float>(i - first_language) * 70;
            const Rect row{panel.x + 28, y, panel.w - 56, 60};
            if (i == language_picker_focus_)
                popup.rounded_rect(row, 12, palette_.accent.with_alpha(0.22f));
            const auto &language = prospero_language::chatterbox_language(static_cast<unsigned>(i));
            text(popup, language.name, row.x + 20, row.y + 22, 24,
                 i == language_picker_focus_ ? palette_.ink : palette_.muted,
                 i == language_picker_focus_);
            text(popup, language.code, row.x + row.w - 20, row.y + 22, 21, palette_.muted, false,
                 gfx::Align::right);
        }
        popup.line(panel.x + 28, panel.y + panel.h - 88, panel.x + panel.w - 28,
                   panel.y + panel.h - 88, 1, palette_.line);
        ui::Hint hints[3]{{ui::Button::dpad, "Select"},
                          {ui::Button::cross, "Use language"},
                          {ui::Button::circle, "Back"}};
        auto glyphs = palette_.day ? ui::GlyphStyle::light() : ui::GlyphStyle::dark();
        glyphs.label = palette_.ink;
        ui::HintLayout layout;
        layout.size = 30;
        layout.text_size = 21;
        layout.cy = panel.y + panel.h - 43;
        layout.item_gap = 26;
        const float width = ui::measure_hints(fonts_, hints, 3, layout);
        ui::draw_hints(popup, fonts_, glyphs, hints, 3, panel.cx() - width / 2, false, layout);
    }
    if (voice_recording_open_)
    {
        frame.glass = true;
        auto &popup = frame.overlay;
        popup.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                           Color::rgb(0x000000, 0.72f));
        const Rect panel{530, 270, 860, 540};
        popup.bordered_rect(panel, 24, palette_.panel.with_alpha(1.0f / palette_.panel.a), 1,
                            palette_.line);
        label(popup, "MY VOICE", panel.x + 42, panel.y + 52, palette_.accent);
        const auto state = voice_recorder_.state();
        text(popup,
             state == VoiceRecorder::State::Recording ? "Recording your voice…"
             : state == VoiceRecorder::State::Ready   ? "Listen, then save your voice"
                                                      : "Record a voice sample",
             panel.x + 42, panel.y + 110, 32, palette_.ink, true);
        text(popup, "Speak clearly for up to five seconds.", panel.x + 42, panel.y + 175, 24,
             palette_.ink);
        text(popup, "Uses your PS5 input device. Unmute the DualSense microphone.", panel.x + 42,
             panel.y + 218, 21, palette_.muted);
        const float progress = std::min(1.0f, voice_recorder_.seconds() / 5.0f);
        popup.rounded_rect({panel.x + 42, panel.y + 270, 776, 14}, 7, palette_.line);
        if (progress > 0)
            popup.rounded_rect({panel.x + 42, panel.y + 270, 776 * progress, 14}, 7,
                               palette_.accent);
        char duration[80];
        std::snprintf(duration, sizeof(duration), "%.1f / 5.0 seconds", voice_recorder_.seconds());
        text(popup, duration, panel.x + 42, panel.y + 320, 24, palette_.ink);
        const std::string message = state == VoiceRecorder::State::Failed
                                        ? voice_recorder_.error()
                                        : voice_recording_feedback_;
        text(popup, message, panel.x + 42, panel.y + 380, 20, palette_.warn);
        popup.line(panel.x + 28, panel.y + panel.h - 76, panel.x + panel.w - 28,
                   panel.y + panel.h - 76, 1, palette_.line);
        if (app_.busy())
            text(popup, "Playing your sample…", 960, panel.y + panel.h - 34, 21, palette_.muted,
                 false, gfx::Align::center);
        else
        {
            ui::Hint hints[4];
            int count = 0;
            hints[count++] = {ui::Button::cross, state == VoiceRecorder::State::Recording ? "Stop"
                                                 : state == VoiceRecorder::State::Ready
                                                     ? "Save and use"
                                                     : "Start recording"};
            if (state == VoiceRecorder::State::Ready)
            {
                hints[count++] = {ui::Button::square, "Listen"};
                hints[count++] = {ui::Button::triangle, "Record again"};
            }
            hints[count++] = {ui::Button::circle, "Cancel"};
            auto glyphs = palette_.day ? ui::GlyphStyle::light() : ui::GlyphStyle::dark();
            glyphs.label = palette_.ink;
            ui::HintLayout layout;
            layout.size = 28;
            layout.text_size = 20;
            layout.cy = panel.y + panel.h - 34;
            layout.item_gap = 22;
            const float width = ui::measure_hints(fonts_, hints, count, layout);
            ui::draw_hints(popup, fonts_, glyphs, hints, count, panel.cx() - width / 2, false,
                           layout);
        }
    }
    draw_update_notes(overlay);
    draw_update(overlay);
    if (update_dialog_.visible())
    {
        frame.glass = true;
        ui::Canvas frosted{frame.overlay, fonts_, frame.glass_texture, clock_};
        update_dialog_.draw(frosted);
    }
    toasts_.draw(overlay);
}

void NativeUI::Impl::draw_header(ui::Canvas &canvas) const
{
    auto &list = canvas.list;
    draw_mark(list, 119, 91, 1.05f, palette_.accent);
    text(list, "ProsperoAI", 159, 101, 29, palette_.ink, true);

    const auto glyphs = palette_.day ? ui::GlyphStyle::light() : ui::GlyphStyle::dark();
    const float shoulder = ui::button_width(ui::Button::l1, 30);
    ui::draw_button(list, fonts_, glyphs, ui::Button::l1, tabs_left_ - shoulder - 26, 90, 30);
    tabs_.draw(canvas);
    ui::draw_button(list, fonts_, glyphs, ui::Button::r1, tabs_left_ + tabs_width_ + 30, 90, 30);

    // What can answer, and what it is doing.
    const auto &state = app_.state();
    const bool busy = app_.busy();
    const Color tone = busy                                             ? palette_.warn
                       : state.selected_model >= 0 && state.ready       ? palette_.good
                       : state.selected_model < 0 && !state.initialized ? palette_.muted
                                                                        : palette_.bad;
    std::string status = activity_text();
    char loaded_id[256]{}, loaded_name[256]{};
    const bool loaded =
        gpt_runtime_loaded_model(loaded_id, sizeof(loaded_id), loaded_name, sizeof(loaded_name));
    const std::string memory =
        loaded ? "Memory: " + fit(loaded_name[0] ? loaded_name : loaded_id, 19, 330)
               : "Memory: no model loaded";
    const float memory_width = fonts_.regular.measure(memory, 19);
    text(list, memory, kRight - memory_width, 126, 19, loaded ? palette_.good : palette_.muted);
    const float width = chip_width(status, true);
    chip(list, kRight, 90, status, tone, true, true);
    if (busy && !reduced_motion())
        list.ring(kRight - width + 20, 90, 4 + 7 * ui::breathe(clock_, 1.2f), 1.5f,
                  tone.with_alpha(0.5f * (1 - ui::breathe(clock_, 1.2f))));
    list.line(kLeft, kHeaderRule, kRight, kHeaderRule, 1, palette_.line);
}

void NativeUI::Impl::draw_footer(gfx::DrawList &list) const
{
    list.line(kLeft, kFooterRule, kRight, kFooterRule, 1, palette_.line);
    if (kokoro_voice_picker_open_ || language_picker_open_ || voice_recording_open_)
        return;
    const auto &state = app_.state();
    ui::Hint hints[8];
    int count = 0;
    if (dialog_.is_open() || update_ui_ == UpdateUi::offer || update_ui_ == UpdateUi::failed)
    {
        hints[count++] = {ui::Button::cross, "Choose"};
        hints[count++] = {ui::Button::circle, "Back"};
    }
    else if (update_ui_ != UpdateUi::hidden)
    {
        // The update's own panels name their buttons.
    }
    else if (search_open_ || chat_keyboard_open_)
    {
        hints[count++] = {ui::Button::cross, "Type"};
        hints[count++] = {ui::Button::triangle, "Space"};
        hints[count++] = {ui::Button::l2, "Symbols"};
        hints[count++] = {ui::Button::r2, chat_keyboard_open_ ? "Send" : "Done"};
        hints[count++] = {ui::Button::circle, "Back"};
    }
    else if (tabs_.active() == 1)
    {
        const bool downloading =
            prospero_model_download::state() == prospero_model_download::State::Downloading;
        bool cross_cancels = false;
        if (filters_.active() == 0)
        {
            const auto download_state = prospero_model_download::state();
            const bool has_results = prospero_model_download::candidate_count() > 0;
            cross_cancels = downloading;
            const char *action =
                downloading ? "Cancel download"
                : has_results && download_state == prospero_model_download::State::Ready
                    ? "Download"
                : has_results && download_state == prospero_model_download::State::SearchReady
                    ? "Browse files"
                : download_state == prospero_model_download::State::Ready ||
                        download_state == prospero_model_download::State::SearchReady
                    ? "Edit search"
                : search_.length() < 2 ? "Enter search"
                                       : "Search";
            hints[count++] = {ui::Button::cross, action};
            hints[count++] = {ui::Button::square, "Edit search"};
        }
        else if (!visible_models_.empty())
        {
            const int focused = visible_models_[static_cast<std::size_t>(models_.focus())];
            cross_cancels =
                focused < -1 && prospero_model_download::active_preset() == -focused - 2;
            hints[count++] = {ui::Button::cross, focused >= 0                  ? "Use model"
                                                 : cross_cancels               ? "Cancel download"
                                                 : preset_saved_[-focused - 2] ? "Details"
                                                                               : "Download"};
        }
        if (downloading && !cross_cancels)
            hints[count++] = {ui::Button::triangle, "Cancel download"};
        if (filters_.active() != 0)
        {
            hints[count++] = {ui::Button::square, "Search"};
        }
    }
    else if (tabs_.active() == 3)
    {
        hints[count++] = {ui::Button::l1, "Settings"};
        hints[count++] = {ui::Button::r1, "Workspace"};
    }
    else if (tabs_.active() == 2)
    {
        hints[count++] = {ui::Button::cross, category_focus_ ? "Open" : "Adjust"};
        if (!category_focus_)
            hints[count++] = {ui::Button::circle, "Categories"};
    }
    else if (workspace_generation_open_)
    {
        hints[count++] = {ui::Button::circle, "Back"};
        hints[count++] = {ui::Button::dpad, "Adjust"};
    }
    else if (!conversation_)
    {
        hints[count++] = {ui::Button::cross, "Start Conversation"};
    }
    else if (rail_)
    {
        hints[count++] = {ui::Button::cross, selecting_model_ ? "Use model" : "Open"};
        hints[count++] = {ui::Button::circle, "Back"};
        hints[count++] = {ui::Button::square, "Search"};
        hints[count++] = {ui::Button::l2, "Type"};
        hints[count++] = {ui::Button::r2, "Type"};
    }
    else
    {
        const bool audio = std::any_of(chat_rows_.begin(), chat_rows_.end(),
                                       [](const auto &r) { return r.audio; });
        hints[count++] = {ui::Button::cross, composer_.text().empty() ? "Write" : "Send"};
        if (state.retry_available)
            hints[count++] = {ui::Button::triangle, "Retry"};
        else if (audio)
            hints[count++] = {ui::Button::triangle, "Play visible audio"};
        hints[count++] = {ui::Button::square, "New"};
        hints[count++] = {ui::Button::right_stick, "Scroll"};
    }
    if (!dialog_.is_open() && !search_open_ && !chat_keyboard_open_ &&
        update_ui_ == UpdateUi::hidden)
        hints[count++] = {ui::Button::options, "Close"};
    if (!dialog_.is_open() && !search_open_ && !chat_keyboard_open_ &&
        update_ui_ == UpdateUi::hidden)
    {
        if (tabs_.active() == 1)
            hints[count++] = {ui::Button::left_stick, "L3  Delete model"};
        else if (conversation_ && rail_ && selecting_model_ && !workspace_model_indices_.empty())
            hints[count++] = {ui::Button::left_stick, "L3  Delete model"};
        else if (conversation_ && rail_ && !selecting_model_ && sessions_.focus() == 0 &&
                 app_.state().selected_model >= 0)
            hints[count++] = {ui::Button::left_stick, "L3  Delete model"};
        else if (conversation_ && rail_ && !selecting_model_ &&
                 sessions_.focus() >= workspace_sessions_row_ &&
                 static_cast<std::size_t>(sessions_.focus() - workspace_sessions_row_) <
                     visible_sessions_.size())
            hints[count++] = {ui::Button::left_stick, "L3  Delete conversation"};
    }
    auto glyphs = palette_.day ? ui::GlyphStyle::light() : ui::GlyphStyle::dark();
    glyphs.label = palette_.ink;
    ui::HintLayout layout;
    layout.size = 32;
    layout.text_size = 22;
    layout.cy = 1012;
    layout.item_gap = 34;
    ui::draw_hints(list, fonts_, glyphs, hints, count, kRight, true, layout);
    label(list, "PRIVATE BY DESIGN", kLeft, 1019, palette_.muted);
}

void NativeUI::Impl::draw_boot(gfx::DrawList &list) const
{
    const float veil = reduced_motion() ? boot_ : tween::cubic_out(boot_);
    list.push_opacity(veil);
    list.gradient_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, palette_.page_top,
                       palette_.page_bottom);
    // The doors part to reveal the warm light beyond the ProsperoAI arch.
    const float opening =
        reduced_motion() ? 1.0f
                         : tween::cubic_out(std::clamp((boot_age_ - 0.12f) / 0.95f, 0.0f, 1.0f));
    const float lift = (1 - veil) * -24;
    constexpr float center = 960.0f;
    constexpr float floor = 742.0f;
    const float side = 220.0f;
    const float arch_y = 438.0f + lift;
    const float arch_rx = 220.0f;
    const float arch_ry = 214.0f;
    const Color gold = palette_.accent;
    list.glow({center - (10 + 120 * opening), arch_y - 110, 20 + 240 * opening, 350}, 30, 145,
              gold.with_alpha(0.08f + 0.13f * opening));

    // Nested arch strokes stay fixed while the two inset door leaves slide outward.
    for (int ring = 0; ring < 3; ++ring)
    {
        const float inset = static_cast<float>(ring) * 18.0f;
        const float rx = arch_rx - inset;
        const float ry = arch_ry - inset;
        float last_x = center - rx;
        float last_y = arch_y;
        for (int step = 1; step <= 32; ++step)
        {
            const float angle = 3.14159265f + 3.14159265f * static_cast<float>(step) / 32.0f;
            const float x = center + std::cos(angle) * rx;
            const float y = arch_y + std::sin(angle) * ry;
            list.line(last_x, last_y, x, y, ring == 0 ? 3.0f : 1.4f,
                      gold.with_alpha((ring == 0 ? 0.82f : 0.38f) * veil));
            last_x = x;
            last_y = y;
        }
        list.line(center - rx, arch_y, center - rx, floor, ring == 0 ? 3.0f : 1.4f,
                  gold.with_alpha((ring == 0 ? 0.82f : 0.38f) * veil));
        list.line(center + rx, arch_y, center + rx, floor, ring == 0 ? 3.0f : 1.4f,
                  gold.with_alpha((ring == 0 ? 0.82f : 0.38f) * veil));
    }

    const float left_outer = center - side;
    const float right_outer = center + side;
    const float left_top = center - 10 - 140 * opening;
    const float left_bottom = center - 10 - 122 * opening;
    const float right_top = center + 10 + 140 * opening;
    const float right_bottom = center + 10 + 122 * opening;
    const float door_top = arch_y;
    const Color door = gfx::mix(palette_.page_top, palette_.depth, 0.38f).with_alpha(0.92f * veil);
    const float left_leaf[] = {left_outer,  door_top,  left_top,   door_top + 12,
                               left_bottom, floor - 8, left_outer, floor};
    const float right_leaf[] = {right_top,   door_top + 12, right_outer,  door_top,
                                right_outer, floor,         right_bottom, floor - 8};
    list.polygon(left_leaf, 4, door);
    list.polygon(right_leaf, 4, door);
    list.line(left_outer, door_top, left_top, door_top + 12, 2.0f, gold.with_alpha(0.82f * veil));
    list.line(left_top, door_top + 12, left_bottom, floor - 8, 2.0f, gold.with_alpha(0.82f * veil));
    list.line(left_bottom, floor - 8, left_outer, floor, 2.0f, gold.with_alpha(0.82f * veil));
    list.line(right_top, door_top + 12, right_outer, door_top, 2.0f, gold.with_alpha(0.82f * veil));
    list.line(right_outer, door_top, right_outer, floor, 2.0f, gold.with_alpha(0.82f * veil));
    list.line(right_outer, floor, right_bottom, floor - 8, 2.0f, gold.with_alpha(0.82f * veil));
    const float seam = 1 - opening;
    if (seam > 0.01f)
        list.glow({center - 3, door_top + 70, 6, 190}, 3, 38, gold.with_alpha(0.20f * seam * veil));
    list.circle(left_top + 16, 555 + lift, 3.2f, gold.with_alpha(0.8f * veil));
    list.circle(right_top - 16, 555 + lift, 3.2f, gold.with_alpha(0.8f * veil));

    const float words =
        reduced_motion() ? 1 : tween::cubic_out(std::clamp((boot_age_ - 0.25f) / 0.6f, 0.0f, 1.0f));
    list.push_opacity(words);
    ui::text(list, fonts_.display, "ProsperoAI", 960, 810 + (1 - words) * 14 + lift, 64,
             palette_.ink, gfx::Align::center);
    label(list, "RUNS ON YOUR CONSOLE.  STAYS ON YOUR CONSOLE.", 960, 860 + lift, palette_.accent,
          gfx::Align::center);
    const auto &state = app_.state();
    text(list, fit(state.status, 23, 900, false), 960, 916, 23, palette_.muted, false,
         gfx::Align::center);
    // A light travels along the rule while the catalogue is read.
    const Rect track{760, 950, 400, 3};
    list.rounded_rect(track, 1.5f, palette_.line);
    const float travel = reduced_motion() ? 0.5f : std::fmod(boot_age_ * 0.9f, 1.0f);
    list.push_clip(track);
    list.rounded_rect({track.x - 120 + travel * (track.w + 120), track.y, 120, 3}, 1.5f,
                      palette_.accent);
    list.pop_clip();
    text(list, PROSPERO_VERSION, 960, 1012, 19, palette_.muted.with_alpha(0.7f), false,
         gfx::Align::center);
    list.pop_opacity();
    list.pop_opacity();
}

void NativeUI::Impl::draw_search(ui::Canvas &canvas) const
{
    auto &overlay = canvas.list;
    overlay.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                         Color::rgb(0x000000, palette_.day ? 0.45f : 0.72f));
    const Rect sheet{324, 440, 1272, 500};
    overlay.shadow({sheet.x, sheet.y + 18, sheet.w, sheet.h}, 26, 60, Color::rgb(0x000000, 0.45f));
    overlay.bordered_rect(sheet, 26, palette_.panel.with_alpha(1.0f / palette_.panel.a), 1,
                          palette_.line);
    label(overlay,
          chat_keyboard_open_      ? "WRITE A MESSAGE"
          : tabs_.active() == 0    ? "SEARCH CONVERSATIONS"
          : filters_.active() == 0 ? "FIND A MODEL TO DOWNLOAD"
                                   : "SEARCH YOUR MODELS",
          364, 492, palette_.accent);
    const std::string &value = chat_keyboard_open_   ? composer_.text()
                               : tabs_.active() == 0 ? session_search_.text()
                                                     : search_.text();
    const bool empty = value.empty();
    text(overlay,
         empty ? (chat_keyboard_open_ ? "Type your message..." : "Type to search...")
               : fit(value, 32, 900).c_str(),
         364, 548, 32, empty ? palette_.muted : palette_.ink, !empty);
    char found[64];
    std::snprintf(found, sizeof(found), "%zu found",
                  tabs_.active() == 0 ? visible_sessions_.size() : visible_models_.size());
    if (!chat_keyboard_open_ && (tabs_.active() == 0 || filters_.active() != 0))
        text(overlay, found, sheet.x + sheet.w - 40, 548, 22, palette_.muted, false,
             gfx::Align::right);
    keyboard_.draw(canvas);
}

NativeUI::NativeUI(App &app, FontSet &fonts, gfx::Renderer &renderer)
    : impl_(std::make_unique<Impl>(*this, app, fonts, renderer))
{
}
NativeUI::~NativeUI()
{
    release();
}
void NativeUI::release()
{
    impl_->release();
}
void NativeUI::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    impl_->update(input, dt, feedback);
}
void NativeUI::draw(UiFrame &frame) const
{
    impl_->draw(frame);
}
void NativeUI::type(char character, ui::Feedback &feedback)
{
    impl_->type(character, feedback);
}
void NativeUI::backspace(ui::Feedback &feedback)
{
    impl_->backspace(feedback);
}
void NativeUI::submit(ui::Feedback &feedback)
{
    impl_->submit(feedback);
}
void NativeUI::scroll(float pixels)
{
    impl_->chat_.scroll_by(0, pixels);
}
void NativeUI::keyboard_result(const char *text)
{
    impl_->keyboard_result(text);
}
bool NativeUI::quit_requested() const
{
    return impl_->quit_;
}
const char *NativeUI::update_state() const
{
    return impl_->update_state();
}
} // namespace prospero
