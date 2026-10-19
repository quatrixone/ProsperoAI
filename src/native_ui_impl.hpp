// ProsperoAI native frontend: the state behind NativeUI, shared by its source files.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_ui.hpp"
#include "model_downloader_ps5.hpp"
#include "update.hpp"
#include "voice_recorder.hpp"
#include "ui/components/dialog.hpp"
#include "ui/components/form.hpp"
#include "ui/components/grid.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/components/list.hpp"
#include "ui/components/progress.hpp"
#include "ui/components/scroll_area.hpp"
#include "ui/components/search_field.hpp"
#include "ui/components/tabs.hpp"
#include "ui/components/text_field.hpp"
#include "ui/components/text_view.hpp"
#include "ui/components/toast.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <map>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prospero
{
// The Settings categories that are more than a form.
constexpr int kDiagnostics = 3, kAbout = 4;
// Where the debug log is written, as the console names the folder.
std::string logs_folder_text();

using namespace hui;
using gfx::Color;
using gfx::Rect;

// ---- layout: the 1920 x 1080 canvas, 96 px safe margins ----
constexpr float kLeft = 96, kRight = 1824, kHeaderRule = 150, kFooterRule = 966;
constexpr float kPi = 3.14159265f;

// Every colour the interface draws with, for one theme, accent and contrast setting.
struct Palette
{
    bool day = false;
    Color page_top, page_bottom; // the backdrop's base
    Color panel, raised;         // tinted surfaces over the backdrop
    Color ink, muted, line;
    Color accent, on_accent;
    Color good, warn, bad;
    Color depth;    // a cool tone the backdrop sets against the accent
    Color kinds[4]; // conversation, image, sound, voice
};
Palette make_palette(const Preferences &preferences);
ui::Theme make_theme(const Palette &palette, const Preferences &preferences);

// Contour sculpture made entirely from the kit's SDF lines. No image assets.
void draw_sculpture(gfx::DrawList &list, float cx, float cy, float size, Color color, float time,
                    int bands);
// The app's mark: four nested arcs.
void draw_mark(gfx::DrawList &list, float cx, float cy, float scale, Color color);

struct NativeUI::Impl
{
    Impl(NativeUI &owner, App &app, FontSet &font_set, gfx::Renderer &renderer);

    // ---- native_ui.cpp: the frame ----
    void update(const InputFrame &input, float dt, ui::Feedback &feedback);
    void draw(UiFrame &frame) const;
    void sync();
    void apply_theme();
    void announce(ui::Feedback &feedback);
    void handle_workspace(const InputFrame &input, ui::Feedback &feedback);
    void handle_models(const InputFrame &input, ui::Feedback &feedback);
    void handle_settings(const InputFrame &input, ui::Feedback &feedback);
    void open_dialog(int action, int index, std::string title, std::string body, const char *button,
                     bool destructive, ui::Feedback &feedback, const char *other = "Not now");
    void change_page(int page);
    void refresh_models();
    void refresh_sessions();
    void refresh_mobile_link();
    void begin_model_selection();
    void build_form();
    void build_generation_form();
    void apply_form();
    void apply_generation_form();
    void type(char character, ui::Feedback &feedback);
    void backspace(ui::Feedback &feedback);
    void submit(ui::Feedback &feedback);
    void keyboard_result(const char *value);
    void upload_visible_image();
    int selected_audio_row() const;
    void release();
    void draw_header(ui::Canvas &canvas) const;
    void draw_footer(gfx::DrawList &list) const;
    void draw_boot(gfx::DrawList &list) const;
    void draw_search(ui::Canvas &canvas) const;

    // ---- native_ui_update.cpp: a newer version, its notes, the update at work ----
    void setup_update();
    void take_update_offer(ui::Feedback &feedback);
    void open_update_offer(ui::Feedback &feedback, bool on_notes);
    void open_update_failure(const char *reason, ui::Feedback &feedback);
    void begin_update(ui::Feedback &feedback);
    void update_modal(const InputFrame &input, float dt, ui::Feedback &feedback);
    void update_tick(float dt);
    void draw_update(ui::Canvas &canvas) const;
    void draw_update_notes(ui::Canvas &canvas) const;
    const char *update_state() const;

    // ---- native_ui_screens.cpp ----
    void draw_welcome(ui::Canvas &canvas) const;
    void draw_conversation(ui::Canvas &canvas) const;
    void draw_models(ui::Canvas &canvas) const;
    void draw_model_filters(ui::Canvas &canvas) const;
    void draw_download_progress(gfx::DrawList &list, const Rect &bounds) const;
    void draw_model(gfx::DrawList &list, const Rect &cell, int index, float focus) const;
    void draw_model_art(gfx::DrawList &list, int kind, float cx, float cy, float size, Color color,
                        float phase, float focus) const;
    void draw_settings(ui::Canvas &canvas) const;
    void draw_mobile(ui::Canvas &canvas) const;

    // ---- native_ui_style.cpp: small drawing helpers ----
    float text(gfx::DrawList &list, std::string_view value, float x, float y, float size,
               Color color, bool bold = false, gfx::Align align = gfx::Align::left) const;
    void label(gfx::DrawList &list, std::string_view value, float x, float y, Color color,
               gfx::Align align = gfx::Align::left) const;
    void panel(gfx::DrawList &list, const Rect &r, float radius = 22, float lit = 0) const;
    // A pill with an optional status dot. Returns its width; right_edge places it by its end.
    float chip(gfx::DrawList &list, float x, float cy, std::string_view value, Color color,
               bool dot, bool right_edge = false) const;
    float chip_width(std::string_view value, bool dot) const;
    void spinner(gfx::DrawList &list, float cx, float cy, float radius, Color color) const;
    void thinking_dots(gfx::DrawList &list, float x, float cy, Color color) const;
    std::string fit(std::string_view value, float size, float width, bool bold = true) const;
    bool reduced_motion() const
    {
        return app_.state().preferences.reduced_motion;
    }
    const char *current_name() const;
    Color kind_color(Capability capability) const
    {
        return palette_.kinds[static_cast<unsigned>(capability)];
    }
    // 0..1 arrival of a part of the current page, `order` steps after the page opened.
    float arrival(int order) const;
    std::string stats_line() const;
    const char *activity_text() const;

    NativeUI &owner_;
    App &app_;
    FontSet &font_set_;
    const ui::Fonts &fonts_;
    gfx::Renderer &renderer_;
    Palette palette_;
    ui::Theme theme_;
    ui::SpringColor cloud_a_, cloud_b_, page_top_, page_bottom_;

    ui::TabBar tabs_, filters_;
    ui::ListView sessions_, categories_;
    ui::GridView models_;
    ui::SearchField search_, session_search_;
    std::vector<unsigned> visible_sessions_;
    std::vector<unsigned> workspace_model_indices_;
    std::vector<std::uint8_t> mobile_qr_modules_;
    int mobile_qr_size_ = 0;
    std::string mobile_url_;
    int workspace_voice_row_ = -1;
    int workspace_language_row_ = -1;
    int workspace_new_conversation_row_ = -1;
    int workspace_sessions_row_ = 3;
    unsigned session_category_ = 0;
    std::string session_query_;
    std::string workspace_model_query_;
    int workspace_model_filter_ = -1; // -1 = all capabilities; otherwise Capability ordinal.
    ui::Keyboard keyboard_;
    ui::Form form_;
    ui::Dialog dialog_;
    ui::TextField composer_;
    ui::ScrollArea chat_;
    ui::ToastStack toasts_;
    // The update: the question, the notes, then the ring while the helper works.
    enum class UpdateUi : std::uint8_t
    {
        hidden,
        offer,
        notes, // What's new: the release notes
        working,
        closing, // staged: the app is about to close
        failed,
    };
    UpdateUi update_ui_ = UpdateUi::hidden;
    update::Offer update_offer_;
    update::Progress update_progress_;
    ui::Dialog update_dialog_;
    ui::ProgressRing update_ring_;
    ui::TextView update_notes_;
    float update_fade_ = 0, update_notes_fade_ = 0, update_closing_age_ = 0;
    float tabs_left_ = 0, tabs_width_ = 0;

    std::vector<int> visible_models_;
    struct ChatRow
    {
        std::string text, role, image_path, audio_path;
        bool audio = false;
        std::vector<std::string> lines;
        float y = 0, height = 0, width = 0, age = 10;
    };
    struct Texture
    {
        std::uint32_t id;
        float aspect;
        float last_used;
    };
    std::map<std::string, Texture> images_;
    std::vector<ChatRow> chat_rows_;

    float clock_ = 0, page_age_ = 10, body_size_ = 28, session_sync_age_ = 0;
    float boot_ = 1, boot_age_ = 0, busy_seconds_ = 0, hero_age_ = 10;
    bool welcomed_ = false, was_busy_ = false, selecting_model_ = false;
    bool kokoro_voice_picker_open_ = false;
    int kokoro_voice_picker_focus_ = 0;
    bool voice_picker_chatterbox_ = false;
    bool language_picker_open_ = false;
    int language_picker_focus_ = 0;
    bool voice_recording_open_ = false;
    VoiceRecorder &voice_recorder_ = voice_recorder();
    bool voice_recording_remote_ = false;
    std::string voice_recording_feedback_;
    std::string voice_delete_id_;
    bool conversation_ = false, search_open_ = false, chat_keyboard_open_ = false, rail_ = false,
         category_focus_ = false, workspace_generation_open_ = false,
         conversation_restore_pending_ = false;
    bool empty_models_download_focus_ = true;
    bool keyboard_pending_ = false, quit_ = false;
    int download_focus_ = 0;
    prospero_model_download::State last_download_state_ = prospero_model_download::State::Idle;
    bool downloaded_models_pending_ = false, cancel_pending_ = false;
    std::array<bool, prospero_model_download::preset_count> preset_saved_{};
    unsigned revision_ = ~0U, font_revision_ = 0;
    int active_model_ = -1, catalog_count_ = -1, dialog_action_ = 0, dialog_index_ = -1;
    int category_ = 0, pending_model_ = -1, hero_model_ = -1;
    ui::Pulse send_pulse_;
};
} // namespace prospero
