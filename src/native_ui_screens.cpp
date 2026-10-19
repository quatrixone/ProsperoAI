// ProsperoAI native frontend: what each page draws.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_ui_impl.hpp"
#include "http_server.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace prospero
{
namespace
{
constexpr const char *kKinds[] = {"CONVERSATION", "IMAGE", "SOUND", "VOICE"};
constexpr const char *kKindNames[] = {"Converse", "Imagine", "Compose", "Speak"};
constexpr const char *kKindTags[] = {"01 / TEXT", "02 / IMAGE", "03 / SOUND", "04 / VOICE"};
constexpr const char *kKindLines[] = {"Conversation model", "Image model", "Sound model",
                                      "Voice model"};

// Draws one part of a page as it arrives: faded and a few pixels low until it settles.
template <typename Draw> void part(gfx::DrawList &list, float arrival, Draw &&draw)
{
    list.push_opacity(arrival);
    list.push_transform(1, 0, 0, 0, (1 - arrival) * 16);
    draw();
    list.pop_transform();
    list.pop_opacity();
}
} // namespace

void NativeUI::Impl::draw_model_art(gfx::DrawList &list, int kind, float cx, float cy, float size,
                                    Color color, float phase, float focus) const
{
    if (kind == 0)
        draw_sculpture(list, cx, cy, size, color, clock_ + phase, size > 100 ? 30 : 18);
    else if (kind == 1)
    {
        // Sheets of an image coming into register.
        const float unit = size / 78;
        const float drift = reduced_motion() ? 0 : 0.05f * std::sin(clock_ * 0.6f + phase);
        for (int i = 0; i < 7; ++i)
        {
            const float f = static_cast<float>(i);
            list.rotated_rect({cx - (48 - f * 3) * unit, cy - (48 - f * 2) * unit,
                               (92 - f * 5) * unit, (92 - f * 5) * unit},
                              12 * unit, -0.4f + f * 0.13f + drift * (7 - f),
                              color.with_alpha(0.14f + f * 0.05f));
        }
    }
    else if (kind == 2)
    {
        // A waveform; it breathes a little when it has the focus.
        const float unit = size / 78;
        for (int i = 0; i < 25; ++i)
        {
            const float x = static_cast<float>(i - 12);
            const float live =
                reduced_motion() ? 0 : focus * 0.25f * std::sin(clock_ * 3.2f + x * 0.9f);
            const float h =
                (12 + 52 * std::exp(-x * x / 68) *
                          (0.48f + 0.52f * std::abs(std::sin(x * 0.7f + phase)) + live)) *
                unit;
            list.rounded_rect({cx + x * 5 * unit - 1.5f * unit, cy - h, 3 * unit, h * 2},
                              1.5f * unit, color.with_alpha(0.6f + 0.4f * focus));
        }
    }
    else
    {
        // A voice: a point and the arcs leaving it.
        const float unit = size / 78;
        list.circle(cx - 34 * unit, cy, 9 * unit, color);
        for (int i = 0; i < 4; ++i)
        {
            const float f = static_cast<float>(i);
            const float pulse =
                reduced_motion() ? 0.5f : 0.5f + 0.5f * std::sin(clock_ * 2.4f - f * 0.8f + phase);
            list.arc(cx - 34 * unit, cy, (30 + f * 18) * unit, 3 * unit, 0.5f, 2.14f,
                     color.with_alpha((0.85f - f * 0.17f) * (0.55f + 0.45f * pulse)));
        }
    }
}

void NativeUI::Impl::draw_welcome(ui::Canvas &canvas) const
{
    auto &list = canvas.list;
    const auto &state = app_.state();
    part(list, arrival(1), [&]
         { label(list, "A LITTLE CURIOSITY. ENDLESS POSSIBILITY.", 120, 254, palette_.accent); });
    part(list, arrival(2),
         [&]
         {
             ui::text(list, fonts_.display, "Big ideas.", 114, 382, 96, palette_.ink);
             ui::text(list, fonts_.display, "Right here.", 114, 496, 96, palette_.accent);
         });
    part(list, arrival(3),
         [&]
         {
             text(list, "A space to think, imagine, and create.", 120, 560, 29, palette_.muted);
             text(list, "Powered by your PS5. Entirely yours.", 120, 604, 29, palette_.muted);
         });
    // Keep the main action and its status text stable while the catalogue refreshes.
    // Replaying the page entrance fade made “Choose a model” appear to blink.
    {
        const Rect button{120, 652, 372, 74};
        const bool ready = app_.can_send();
        const char *action = app_.busy() ? "Getting ready..." : "Start Conversation";
        if (ready)
            list.glow(button, 37, 30,
                      palette_.accent.with_alpha(0.10f + 0.12f * ui::breathe(clock_, 2.8f)));
        ui::Painter paint(list, fonts_, theme_);
        paint.button(button, action, ui::ButtonKind::primary, {1, 0, !ready && app_.busy()});
        float x = 520;
        if (app_.busy())
        {
            spinner(list, x + 12, 690, 11, palette_.accent);
            x += 38;
        }
        text(list, fit(state.status, 23, 640, false), x, 698, 23, palette_.muted);
    }
    // The sculpture: the app's one piece of art, drawn from lines.
    const float grown = arrival(2);
    list.glow({1240, 330, 300, 220}, 110, 150, palette_.accent.with_alpha(0.07f * grown));
    draw_sculpture(list, 1390, 440, 250 + 60 * grown, palette_.accent.with_alpha(grown), clock_,
                   48);
    part(list, arrival(4),
         [&] { label(list, "THOUGHT, TAKING SHAPE", 1238, 742, palette_.muted); });

    // What is installed, by what it can do.
    for (int i = 0; i < 4; ++i)
    {
        part(list, arrival(5 + i),
             [&]
             {
                 const Rect tile{kLeft + static_cast<float>(i) * 438, 800, 414, 134};
                 const Color kind = palette_.kinds[i];
                 const unsigned count = state.model_counts[static_cast<std::size_t>(i)];
                 const bool current =
                     state.selected_model >= 0 &&
                     static_cast<int>(
                         state.models[static_cast<std::size_t>(state.selected_model)].capability) ==
                         i;
                 panel(list, tile, 22, current ? 0.55f : 0.0f);
                 list.push_clip(tile.inset(1));
                 draw_model_art(list, i, tile.x + tile.w - 78, tile.cy(), 44,
                                kind.with_alpha(count ? 0.9f : 0.35f), static_cast<float>(i) * 9,
                                0);
                 list.pop_clip();
                 label(list, kKindTags[i], tile.x + 26, tile.y + 40, count ? kind : palette_.muted);
                 text(list, kKindNames[i], tile.x + 26, tile.y + 82, 30, palette_.ink, true);
                 char installed[48];
                 if (!state.initialized)
                     std::snprintf(installed, sizeof(installed), "Looking...");
                 else if (count == 0)
                     std::snprintf(installed, sizeof(installed), "No model yet");
                 else
                     std::snprintf(installed, sizeof(installed), "%u model%s installed", count,
                                   count == 1 ? "" : "s");
                 text(list, installed, tile.x + 26, tile.y + 113, 21, palette_.muted);
             });
    }
}

void NativeUI::Impl::draw_conversation(ui::Canvas &canvas) const
{
    auto &list = canvas.list;
    const auto &state = app_.state();
    const bool generating = app_.generating();

    part(list, arrival(1),
         [&]
         {
             panel(list, {kLeft, 196, 362, 740}, 24, rail_ ? 0.35f : 0.0f);
             label(list, "YOUR SPACE", 122, 246, palette_.muted);
             static constexpr const char *titles[] = {"All conversations", "Text", "Image", "Audio",
                                                      "Voice"};
             static constexpr const char *model_filters[] = {
                 "All models", "Text models", "Image models", "Audio models", "Voice models"};
             const std::string model_title =
                 std::string(model_filters[workspace_model_filter_ + 1]) +
                 (workspace_model_query_.empty() ? "" : " · " + workspace_model_query_) +
                 " · L1/R1 type";
             text(list, fit(selecting_model_ ? model_title : titles[session_category_], 31, 310),
                  122, 292, 31, palette_.ink, true);
             sessions_.draw(canvas);
         });

    const char *name = state.session.id[0] ? state.session.model_name : current_name();
    const Capability capability =
        state.selected_model >= 0
            ? state.models[static_cast<std::size_t>(state.selected_model)].capability
            : Capability::Text;
    part(list, arrival(2),
         [&]
         {
             label(list, "WORKSPACE", 536, 232, palette_.accent);
             text(list,
                  fit(state.session.id[0] ? state.session.title : "A new conversation", 46, 1280),
                  536, 290, 46, palette_.ink, true);
             float x = 536;
             x += chip(list, x, 332, fit(name, 19, 420), kind_color(capability), true) + 12;
             if (state.session.id[0] && !app_.can_send() && !app_.busy())
                 chip(list, x, 332, "Read only", palette_.warn, false);
         });

    const float line = body_size_ * 1.48f;
    const float width = chat_.bounds().w;
    chat_.begin(canvas);
    for (std::size_t index = 0; index < chat_rows_.size(); ++index)
    {
        const auto &row = chat_rows_[index];
        if (row.y + row.height < chat_.offset_y() || row.y > chat_.offset_y() + chat_.bounds().h)
            continue;
        const float appear =
            reduced_motion() ? 1 : tween::cubic_out(std::min(row.age / 0.32f, 1.0f));
        list.push_opacity(appear);
        list.push_transform(1, 0, 0, 0, (1 - appear) * 14);
        const bool user = row.role == "user";
        const bool live = generating && index + 1 == chat_rows_.size() && !user;
        float y = 0;
        if (user)
        {
            const float bubble = std::min(row.width + 56, 956.0f);
            const Rect r{width - bubble, row.y, bubble, row.height - 28};
            list.bordered_rect(r, 22, gfx::mix(palette_.raised, palette_.accent, 0.12f), 1,
                               palette_.accent.with_alpha(0.28f));
            y = row.y + 17 + body_size_;
            for (const auto &text_line : row.lines)
            {
                text(list, text_line, r.x + 28, y, body_size_, palette_.ink);
                y += line;
            }
        }
        else
        {
            draw_mark(list, 16, row.y + 22, 0.75f, kind_color(capability));
            text(list, fit(name, 22, 600), 44, row.y + 30, 22, palette_.ink, true);
            y = row.y + 58 + body_size_ * 0.75f;
            if (live && row.text.empty())
                thinking_dots(list, 2, row.y + 78, palette_.accent);
            float caret_x = 0;
            for (const auto &text_line : row.lines)
            {
                caret_x = text(list, text_line, 0, y, body_size_, palette_.ink.with_alpha(0.92f));
                y += line;
            }
            if (live && !row.text.empty() && (reduced_motion() || std::fmod(clock_, 1.0f) < 0.6f))
                list.rounded_rect({caret_x + 6, y - line - body_size_ * 0.78f, 3, body_size_}, 1.5f,
                                  palette_.accent);
            y += 8 - body_size_ * 0.75f;
            if (!row.image_path.empty())
            {
                const auto texture = images_.find(row.image_path);
                if (texture != images_.end())
                {
                    const Rect picture{0, y, std::min(1240.0f, 360 * texture->second.aspect),
                                       std::min(360.0f, 1240 / texture->second.aspect)};
                    list.shadow({picture.x, picture.y + 12, picture.w, picture.h}, 18, 36,
                                Color::rgb(0x000000, 0.35f));
                    list.image(texture->second.id, picture, {0, 0, 1, 1}, Color::rgb(0xffffff), 18);
                }
                else
                {
                    const bool decoded =
                        std::any_of(state.images.begin(), state.images.end(),
                                    [&](const auto &i) { return i->path == row.image_path; });
                    const Rect frame{0, y, 640, 360};
                    list.bordered_rect(frame, 18, palette_.panel, 1, palette_.line);
                    if (decoded)
                        spinner(list, frame.cx(), frame.cy(), 16, palette_.accent);
                    else
                        text(list, "Image preview unavailable", frame.cx(), frame.cy() + 8, 24,
                             palette_.muted, false, gfx::Align::center);
                }
            }
            else if (row.audio)
            {
                const Rect card{0, y, 640, 92};
                const bool selected = static_cast<int>(index) == selected_audio_row();
                if (selected)
                {
                    list.rounded_rect(card, 20, gfx::mix(palette_.panel, palette_.accent, 0.16f));
                    list.bordered_rect(card, 20, Color::rgb(0x000000, 0), 2,
                                       palette_.accent.with_alpha(0.9f));
                }
                else
                    panel(list, card, 20);
                const auto glyphs = palette_.day ? ui::GlyphStyle::light() : ui::GlyphStyle::dark();
                ui::draw_button(list, fonts_, glyphs, ui::Button::triangle, card.x + 26, card.cy(),
                                34);
                text(list, selected ? "Selected audio · press △" : "Play saved audio", card.x + 78,
                     card.cy() + 9, selected ? 23 : 26, selected ? palette_.accent : palette_.ink,
                     true);
                list.push_clip(card.inset(1));
                draw_model_art(list, 2, card.x + card.w - 110, card.cy(), 34,
                               kind_color(capability), static_cast<float>(index), 0);
                list.pop_clip();
            }
        }
        list.pop_transform();
        list.pop_opacity();
    }
    if (chat_rows_.empty())
    {
        const float calm = arrival(3);
        list.push_opacity(calm);
        draw_mark(list, 30, 62, 1.6f, palette_.accent);
        text(list, "What would you like to explore?", 0, 150, 40, palette_.ink, true);
        text(list, "Ask, imagine, or describe. Everything stays on this console.", 0, 198, 26,
             palette_.muted);
        list.pop_opacity();
    }
    chat_.end(canvas);
    chat_.draw(canvas);

    part(list, arrival(3),
         [&]
         {
             // The line above the composer: what is happening, and what the last answer took.
             float x = 536;
             if (generating)
             {
                 spinner(list, x + 11, 813, 10, palette_.accent);
                 x += 34;
                 char elapsed[64];
                 std::snprintf(elapsed, sizeof(elapsed),
                               "Creating on your console  \xC2\xB7  %.0f s",
                               static_cast<double>(busy_seconds_));
                 text(list, elapsed, x, 820, 21, palette_.muted);
             }
             else
             {
                 if (app_.busy())
                 {
                     spinner(list, x + 11, 813, 10, palette_.accent);
                     x += 34;
                 }
                 text(list, fit(state.status, 21, 760, false), x, 820, 21,
                      state.retry_available ? palette_.warn : palette_.muted);
                 const std::string stats = stats_line();
                 if (!stats.empty())
                     ui::text(list, fonts_.mono, stats, kRight, 820, 19, palette_.muted,
                              gfx::Align::right);
             }
             if (send_pulse_.value > 0.01f)
                 list.glow(composer_.bounds(), 16, 30,
                           palette_.accent.with_alpha(0.35f * send_pulse_.value));
             composer_.draw(canvas);
         });

    if (workspace_generation_open_)
    {
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                          Color::rgb(0x05070b, 0.78f));
        label(list, "YOUR SPACE  /  GENERATION", 536, 206, palette_.accent);
        text(list, "Tune this conversation", 536, 244, 32, palette_.ink, true);
        form_.draw(canvas);
    }
}

void NativeUI::Impl::draw_download_progress(gfx::DrawList &list, const Rect &bounds) const
{
    std::uint64_t completed = 0, total = 0;
    prospero_model_download::progress(&completed, &total);
    const float fraction =
        total ? std::clamp(static_cast<float>(completed) / static_cast<float>(total), 0.0f, 1.0f)
              : 0.0f;
    list.rounded_rect(bounds, bounds.h * 0.5f, palette_.line);
    if (fraction > 0)
        list.rounded_rect({bounds.x, bounds.y, bounds.w * fraction, bounds.h}, bounds.h * 0.5f,
                          palette_.accent);
    char label[100];
    std::snprintf(label, sizeof(label), "%u%% · %llu / %llu MiB",
                  static_cast<unsigned>(fraction * 100),
                  static_cast<unsigned long long>(completed / (1024 * 1024)),
                  static_cast<unsigned long long>(total / (1024 * 1024)));
    text(list, label, bounds.x + bounds.w, bounds.y - 12, 19, palette_.accent, false,
         gfx::Align::right);
}

void NativeUI::Impl::draw_model_filters(ui::Canvas &canvas) const
{
    const Rect bounds = filters_.bounds();
    const auto glyphs = palette_.day ? ui::GlyphStyle::light() : ui::GlyphStyle::dark();
    const float width = ui::button_width(ui::Button::l2, 30);
    ui::draw_button(canvas.list, fonts_, glyphs, ui::Button::l2, bounds.x - width - 18, bounds.cy(),
                    30);
    filters_.draw(canvas);
    ui::draw_button(canvas.list, fonts_, glyphs, ui::Button::r2, bounds.x + bounds.w + 18,
                    bounds.cy(), 30);
}

void NativeUI::Impl::draw_models(ui::Canvas &canvas) const
{
    auto &list = canvas.list;
    const auto &state = app_.state();
    if (filters_.active() == 0)
    {
        label(list, "MODEL LIBRARY", kLeft, 222, palette_.accent);
        text(list, "A new model, when you need one.", 92, 300, 56, palette_.ink, true);
        text(
            list,
            "Search model names, choose a repository, and bring a verified GGUF into your library.",
            kLeft, 346, 24, palette_.muted);
        draw_model_filters(canvas);
        const Rect browser{96, 470, 1120, 478};
        const Rect guide{1240, 470, 584, 478};
        panel(list, browser, 26);
        panel(list, guide, 26);
        label(list, "01  /  FIND A MODEL", 128, 518, palette_.accent);
        search_.draw(canvas);
        text(list, "Find a model by name, then browse its available files.", 128, 652, 19,
             palette_.muted);
        char status[192]{};
        prospero_model_download::status(status, sizeof(status));
        const auto downloader_state = prospero_model_download::state();
        if (downloader_state == prospero_model_download::State::Searching ||
            downloader_state == prospero_model_download::State::Loading ||
            downloader_state == prospero_model_download::State::Downloading)
            spinner(list, 140, 700, 9, palette_.accent);
        text(list, status, 162, 708, 21,
             downloader_state == prospero_model_download::State::Failed ? palette_.bad
                                                                        : palette_.muted);
        if (downloader_state == prospero_model_download::State::Downloading)
            draw_download_progress(list, {128, 740, 1056, 12});
        if (downloader_state == prospero_model_download::State::Ready ||
            downloader_state == prospero_model_download::State::SearchReady)
        {
            for (std::size_t i = 0; i < prospero_model_download::candidate_count() && i < 8; ++i)
            {
                prospero_model_download::Candidate candidate{};
                if (!prospero_model_download::candidate(i, &candidate))
                    continue;
                const float y = 758.0f + static_cast<float>(i) * 23.0f;
                const Color ink =
                    static_cast<int>(i) == download_focus_ ? palette_.accent : palette_.ink;
                if (static_cast<int>(i) == download_focus_)
                    list.circle(137, y - 7, 4, ink);
                char row[220];
                const char *fit_text = candidate.fit == prospero_model_download::Fit::Tight
                                           ? "  ·  tight on memory"
                                       : candidate.fit == prospero_model_download::Fit::DoesNotFit
                                           ? "  ·  will not fit"
                                           : "";
                if (downloader_state == prospero_model_download::State::SearchReady)
                    std::snprintf(row, sizeof(row), "%s  ·  %llu downloads  ·  Cross to browse",
                                  candidate.name, static_cast<unsigned long long>(candidate.size));
                else
                    std::snprintf(
                        row, sizeof(row), "%s  ·  %llu MiB%s  ·  Cross to download", candidate.name,
                        static_cast<unsigned long long>(candidate.size / (1024 * 1024)), fit_text);
                text(list, fit(row, 18, 1020, false), 154, y, 18, ink);
            }
        }
        label(list, "02  /  READY FOR YOUR CONSOLE", 1280, 518, palette_.accent);
        draw_sculpture(list, guide.cx(), 646, 68, palette_.accent, clock_, 22);
        text(list, "Browse. Verify. Think.", guide.cx(), 754, 28, palette_.ink, true,
             gfx::Align::center);
        text(list, "GGUF files are checked before they are", guide.cx(), 804, 20, palette_.muted,
             false, gfx::Align::center);
        text(list, "added to the shared model library.", guide.cx(), 835, 20, palette_.muted, false,
             gfx::Align::center);
        text(list, "Large files are flagged tight or will not fit; context also needs memory.",
             guide.cx(), 900, 17, palette_.muted, false, gfx::Align::center);
        return;
    }
    if (state.models.empty() && visible_models_.empty() && search_.text().empty())
    {
        // Nothing to browse: say what to do about it.
        part(list, arrival(1),
             [&]
             {
                 label(list, "MODEL LIBRARY", kLeft, 222, palette_.accent);
                 text(list, "Find your next model.", 92, 300, 52, palette_.ink, true);
                 draw_model_filters(canvas);
                 const Rect download_button{kLeft, 474, 344, 66};
                 list.bordered_rect(download_button, 20, palette_.accent,
                                    empty_models_download_focus_ ? 2.0f : 1.0f,
                                    empty_models_download_focus_
                                        ? palette_.ink.with_alpha(0.78f)
                                        : palette_.accent.with_alpha(0.8f));
                 text(list, "Download a model", download_button.cx(), download_button.cy() + 8, 23,
                      palette_.on_accent, true, gfx::Align::center);
                 text(list, "Cross to browse", kLeft, 578, 21, palette_.muted);
                 text(list, "verified GGUF downloads", kLeft, 610, 21, palette_.muted);
                 const Rect card{460, 488, 1000, 424};
                 panel(list, card, 28);
                 draw_sculpture(list, card.cx(), card.y + 130, 84, palette_.accent, clock_, 24);
                 text(list, state.initialized ? "No models installed yet" : "Looking for models...",
                      card.cx(), card.y + 262, 40, palette_.ink, true, gfx::Align::center);
                 if (state.initialized)
                 {
                     text(list, "Copy your models into the shared folder on this console,",
                          card.cx(), card.y + 316, 25, palette_.muted, false, gfx::Align::center);
                     text(list, "or download a model directly in ProsperoAI.", card.cx(),
                          card.y + 352, 25, palette_.muted, false, gfx::Align::center);
                     ui::text(list, fonts_.mono, "/data/homebrew/prosperoai/models", card.cx(),
                              card.y + 404, 22, palette_.accent, gfx::Align::center);
                 }
             });
        return;
    }

    // The model under the focus leads the page.
    if (hero_model_ >= 0)
    {
        const auto &model = state.models[static_cast<std::size_t>(hero_model_)];
        const int kind = static_cast<int>(model.capability);
        const Color colour = palette_.kinds[kind];
        const float fade =
            reduced_motion() ? 1 : tween::cubic_out(std::min(hero_age_ / 0.24f, 1.0f));
        list.push_opacity(fade);
        list.push_transform(1, 0, 0, (1 - fade) * 18, 0);
        label(list, kKinds[kind], kLeft, 222, colour);
        text(list, fit(model.name, 62, 1180), 92, 300, 62, palette_.ink, true);
        text(list, fit(std::string(kKindLines[kind]) + "  \xC2\xB7  " + model.id, 25, 1180, false),
             kLeft, 346, 25, palette_.muted);
        list.glow({1560, 216, 180, 110}, 60, 90, colour.with_alpha(0.07f));
        draw_model_art(list, kind, 1650, 272, 84, colour, static_cast<float>(hero_model_) * 12, 1);
        list.pop_transform();
        list.pop_opacity();
        if (hero_model_ == pending_model_ && app_.busy())
            chip(list, 1500, 222, "Preparing", palette_.warn, true, true);
        else if (hero_model_ == active_model_)
            chip(list, 1500, 222, state.ready ? "Active" : "Needs attention",
                 state.ready ? palette_.good : palette_.bad, true, true);
    }
    else if (hero_model_ < -1)
    {
        const auto &preset = prospero_model_download::presets[-hero_model_ - 2];
        const Color colour = palette_.kinds[preset.kind];
        label(list, "PRESET MODEL", kLeft, 222, colour);
        text(list, fit(preset.name, 62, 1180), 92, 300, 62, palette_.ink, true);
        text(list,
             preset.kind == 0 ? "Verified GGUF preset · Download directly to your console"
                              : "Prepared model bundle · Download directly to your console",
             kLeft, 346, 25, palette_.muted);
        draw_model_art(list, preset.kind, 1650, 272, 84, colour, 12, 1);
    }
    else
    {
        label(list, "YOUR MODELS", kLeft, 222, palette_.accent);
        text(list, "Nothing matches.", 92, 300, 62, palette_.ink, true);
        text(list, "Try another name, or another kind of model.", kLeft, 346, 25, palette_.muted);
    }

    part(list, arrival(1),
         [&]
         {
             search_.draw(canvas);
             draw_model_filters(canvas);
         });
    part(list, arrival(2), [&] { models_.draw(canvas); });
    char count[96];
    std::snprintf(count, sizeof(count), "%zu shown  /  %zu installed", visible_models_.size(),
                  state.models.size());
    text(list, count, kLeft, 948, 21, palette_.muted);
    char status[192]{};
    prospero_model_download::status(status, sizeof(status));
    const auto download_state = prospero_model_download::state();
    if (download_state == prospero_model_download::State::Downloading)
        draw_download_progress(list, {kLeft, 902, kRight - kLeft, 12});
    const bool show_status = download_state == prospero_model_download::State::Downloading ||
                             download_state == prospero_model_download::State::Complete ||
                             download_state == prospero_model_download::State::Failed;
    text(list,
         show_status ? fit(status, 19, 1200, false)
                     : "Choose a preset to download, or use an installed model.",
         kRight, 948, 19,
         download_state == prospero_model_download::State::Failed ? palette_.bad : palette_.muted,
         false, gfx::Align::right);
}

void NativeUI::Impl::draw_model(gfx::DrawList &list, const Rect &r, int index, float focus) const
{
    const auto &state = app_.state();
    const bool preset_card = index < -1;
    const auto *preset = preset_card ? &prospero_model_download::presets[-index - 2] : nullptr;
    const Model model =
        preset_card ? Model{preset->id, preset->name, "", static_cast<Capability>(preset->kind)}
                    : state.models[static_cast<std::size_t>(index)];
    const int kind = static_cast<int>(model.capability);
    const Color colour = palette_.kinds[kind];
    if (focus > 0.01f)
        list.glow(r, 22, 34, colour.with_alpha(0.16f * focus));
    list.bordered_rect(r, 22, gfx::mix(palette_.panel, palette_.raised, focus * 0.55f), 1,
                       gfx::mix(palette_.line, colour.with_alpha(0.55f), focus));
    list.push_clip(r.inset(1));
    list.gradient_rect_h({r.x + 1, r.y + 1, r.w * 0.55f, r.h - 2}, 22,
                         colour.with_alpha(0.10f + 0.06f * focus), colour.with_alpha(0.0f));
    draw_model_art(list, kind, r.x + 97, r.cy(), 78, colour, static_cast<float>(index) * 12, focus);
    label(list, kKinds[kind], r.x + 196, r.y + 55, colour);
    text(list, fit(model.name, 30, r.w - 224), r.x + 196, r.y + 97, 30, palette_.ink, true);
    char detail[100]{};
    if (preset_card)
        std::snprintf(detail, sizeof(detail), "%llu MiB · %s",
                      static_cast<unsigned long long>(preset->size / (1024 * 1024)),
                      kind == 0 ? "Text generation" : "Prepared bundle");
    text(list, fit(preset_card ? detail : model.id, 20, r.w - 224, false), r.x + 196, r.y + 131, 20,
         palette_.muted);
    if (preset_card)
    {
        if (prospero_model_download::active_preset() == -index - 2)
        {
            spinner(list, r.x + 206, r.y + 163, 8, colour);
            text(list, "Downloading...", r.x + 224, r.y + 170, 20, colour);
        }
        else
        {
            text(list, preset_saved_[-index - 2] ? "Saved" : "Download preset", r.x + 196,
                 r.y + 170, 20, preset_saved_[-index - 2] ? palette_.good : colour);
        }
    }
    else if (index == pending_model_ && app_.busy())
    {
        spinner(list, r.x + 206, r.y + 163, 8, palette_.warn);
        text(list, "Preparing...", r.x + 224, r.y + 170, 20, palette_.warn);
    }
    else if (index == active_model_)
    {
        const Color tone = state.ready ? palette_.good : palette_.bad;
        list.circle(r.x + 202, r.y + 163, 4, tone);
        text(list, state.ready ? "Active" : "Needs attention", r.x + 216, r.y + 170, 20, tone);
    }
    list.pop_clip();
}

void NativeUI::Impl::draw_settings(ui::Canvas &canvas) const
{
    auto &list = canvas.list;
    const auto &state = app_.state();
    part(list, arrival(1),
         [&]
         {
             label(list, "SETTINGS", kLeft, 222, palette_.accent);
             text(list, "Make it feel like yours.", 92, 296, 58, palette_.ink, true);
         });
    part(list, arrival(2),
         [&]
         {
             panel(list, {kLeft, 338, 350, 568}, 24, category_focus_ ? 0.35f : 0.0f);
             categories_.draw(canvas);
         });
    part(list, arrival(3),
         [&]
         {
             panel(list, {470, 338, 960, 568}, 24, category_focus_ ? 0.0f : 0.2f);
             form_.draw(canvas);
         });
    part(list, arrival(4),
         [&]
         {
             const Rect card{1454, 338, 370, 568};
             panel(list, card, 24);
             list.push_clip(card.inset(1));
             if (category_ == kDiagnostics)
             {
                 // What to do with the log, beside its switch.
                 label(list, "REPORTING A PROBLEM", card.x + 30, card.y + 46, palette_.accent);
                 float at = card.y + 92;
                 for (const char *step :
                      {"Turn Debug log on.", "Repeat what went wrong.",
                       "Copy debug-trace.txt from the logs folder with an FTP program and "
                       "send it to us with a line on what you did.",
                       "Turn Debug log off again."})
                     at = ui::paragraph(list, fonts_.regular, step, card.x + 30, at, 21,
                                        card.w - 60, 30, palette_.ink.with_alpha(0.92f), 5) +
                          18;
                 ui::paragraph(list, fonts_.mono, logs_folder_text(), card.x + 30,
                               std::max(at + 6, card.y + 470), 18, card.w - 60, 26, palette_.muted,
                               3);
             }
             else if (category_ == kAbout)
             {
                 // About: the mark, the name and where it runs.
                 draw_sculpture(list, card.cx(), card.y + 150, 92, palette_.accent, clock_, 24);
                 ui::text(list, fonts_.display, "ProsperoAI", card.cx(), card.y + 318, 38,
                          palette_.ink, gfx::Align::center);
                 label(list, "THOUGHT, TAKING SHAPE", card.cx(), card.y + 356, palette_.accent,
                       gfx::Align::center);
                 ui::paragraph(list, fonts_.regular,
                               "Models run on this console's own hardware. No account or cloud "
                               "uploads.",
                               card.x + 30, card.y + 420, 22, card.w - 60, 32, palette_.muted, 4);
                 label(list, "MADE BY", card.cx(), card.y + 520, palette_.accent,
                       gfx::Align::center);
                 ui::text(list, fonts_.semibold, "BlackBearReloaded and Quatrixone", card.cx(),
                          card.y + 545, 18, palette_.accent, gfx::Align::center);
             }
             else
             {
                 // A small conversation in the colours and the reading size being chosen.
                 label(list, "LIVE PREVIEW", card.x + 30, card.y + 46, palette_.accent);
                 const float size = state.preferences.text_size ? 27.0f : 23.0f;
                 const Rect bubble{card.x + 90, card.y + 80, card.w - 120, size + 38};
                 list.bordered_rect(bubble, 18, gfx::mix(palette_.raised, palette_.accent, 0.12f),
                                    1, palette_.accent.with_alpha(0.28f));
                 text(list, "Tell me a story.", bubble.x + 22, bubble.cy() + size * 0.35f, size,
                      palette_.ink);
                 draw_mark(list, card.x + 44, card.y + 190, 0.7f, palette_.accent);
                 text(list, "ProsperoAI", card.x + 70, card.y + 197, 20, palette_.ink, true);
                 const float after = ui::paragraph(
                     list, fonts_.regular, "Once, a small light learned to think for itself.",
                     card.x + 30, card.y + 244, size, card.w - 60, size * 1.45f,
                     palette_.ink.with_alpha(0.92f), 4);
                 const Rect send{card.x + 30, std::max(after + 10, card.y + 400), card.w - 60, 58};
                 ui::Painter paint(list, fonts_, theme_);
                 paint.button(send, "Send", ui::ButtonKind::primary, {0, 0, false});
                 draw_model_art(list, 2, card.cx(), card.y + 512, 36, palette_.accent, 3, 1);
             }
             list.pop_clip();
         });
}

void NativeUI::Impl::draw_mobile(ui::Canvas &canvas) const
{
    auto &list = canvas.list;
    label(list, "YOUR PHONE, YOUR SPACE", 120, 238, palette_.accent);
    text(list, "Open ProsperoAI on mobile", 120, 286, 43, palette_.ink, true);
    text(list, "Scan this code while your phone is on the same network as your PS5.", 120, 332, 24,
         palette_.muted);

    const Rect card{390, 390, 1140, 490};
    panel(list, card, 28, 0.3f);
    const Rect qr_box{438, 420, 430, 430};
    list.rounded_rect(qr_box, 10, Color::rgb(0xffffff));
    if (mobile_qr_size_ > 0 && !mobile_qr_modules_.empty())
    {
        const float module = qr_box.w / static_cast<float>(mobile_qr_size_ + 8);
        const float origin_x = qr_box.x + (qr_box.w - module * mobile_qr_size_) * 0.5f;
        const float origin_y = qr_box.y + (qr_box.h - module * mobile_qr_size_) * 0.5f;
        const Color ink = Color::rgb(0x111318);
        for (int y = 0; y < mobile_qr_size_; ++y)
            for (int x = 0; x < mobile_qr_size_; ++x)
                if (mobile_qr_modules_[static_cast<std::size_t>(y * mobile_qr_size_ + x)])
                    list.rounded_rect({origin_x + static_cast<float>(x) * module,
                                       origin_y + static_cast<float>(y) * module, module, module},
                                      0, ink);
    }
    else
        text(list, "Network address unavailable", qr_box.cx(), qr_box.cy(), 22,
             Color::rgb(0x343943), true, gfx::Align::center);

    const float info_x = 920;
    label(list, "MOBILE WEB APP", info_x, 468, palette_.accent);
    text(list, mobile_url_.empty() ? "Connect your PS5 to a network" : "Scan to open the web UI",
         info_x, 516, 30, palette_.ink, true);
    if (!mobile_url_.empty())
        text(list, fit(mobile_url_, 22, 540, false), info_x, 562, 22, palette_.muted);
    text(list, "Your phone and PS5 must be on the same local network.", info_x, 650, 21,
         palette_.muted);
    const int auth_status = prospero_http_server_auth_status();
    const char *auth_text = auth_status < 0
                                ? "API key configuration is invalid; requests are blocked."
                            : auth_status > 0 ? "API key protection is enabled."
                                              : "API key protection is off.";
    text(list, auth_text, info_x, 684, 21, palette_.muted);
    text(list, "HTTP is not encrypted. Use only a trusted network.", info_x, 718, 21,
         palette_.warn);
    text(list, "Models and generation stay on your console.", info_x, 752, 21, palette_.muted);
}

} // namespace prospero
