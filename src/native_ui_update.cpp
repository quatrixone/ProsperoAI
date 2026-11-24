// ProsperoAI native frontend: a newer version, its release notes and the update at work.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_ui_impl.hpp"

#include "debug_log.hpp"

#include <algorithm>
#include <cstdio>

namespace prospero
{
namespace
{
constexpr Rect kUpdatePanel{520.0f, 230.0f, 880.0f, 600.0f};
constexpr Rect kNotesPanel{400.0f, 120.0f, 1120.0f, 840.0f};
} // namespace

void NativeUI::Impl::setup_update()
{
    update_dialog_.style = dialog_.style;
    update_dialog_.style.width = 940;
    update_ring_.style.thickness = 14;
    update_ring_.set_bounds(
        {kUpdatePanel.x + kUpdatePanel.w * 0.5f - 95, kUpdatePanel.y + 196, 190, 190});
    update_notes_.style.panel = false;
    update_notes_.style.focus_ring = false;
    update_notes_.style.footer = false;
    update_notes_.style.padding = 8;
    update_notes_.style.subheading_size = 27;
    update_notes_.set_bounds(
        {kNotesPanel.x + 56, kNotesPanel.y + 150, kNotesPanel.w - 112, kNotesPanel.h - 270});
    update_notes_.set_active(true);
}

const char *NativeUI::Impl::update_state() const
{
    static constexpr const char *kNames[] = {"none",    "offer",   "notes",
                                             "working", "closing", "failed"};
    return kNames[static_cast<unsigned>(update_ui_)];
}

// The catalog's answer, once it has come: a version the app can install is a question,
// one it cannot is a notice.
void NativeUI::Impl::take_update_offer(ui::Feedback &feedback)
{
    update::Offer offered;
    if (!update::take_offer(&offered))
        return;
    update_offer_ = std::move(offered);
    debug::line("notice", "update: ProsperoAI %s is listed%s", update_offer_.version,
                update_offer_.installable ? "" : ", not installable from here");
    if (!update_offer_.installable)
    {
        toasts_.push(ui::StatusKind::info, TR("update.update_available", "Update available"),
                     i18n::fill(TR("update.is_out_get_it",
                                   "ProsperoAI %s is out. Get it from homebrew.page."),
                                {update_offer_.version}),
                     8.0f);
        return;
    }
    // The notes as an article: list items, callouts, headings and text.
    std::vector<ui::TextBlock> blocks;
    const std::string &notes = update_offer_.notes;
    for (std::size_t at = 0; at < notes.size();)
    {
        std::size_t end = notes.find('\n', at);
        if (end == std::string::npos)
            end = notes.size();
        const std::string line = notes.substr(at, end - at);
        at = end + 1;
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        const auto starts = [&line](const char *prefix) { return line.rfind(prefix, 0) == 0; };
        const char last = line.back();
        font_set_.note(line);
        if (starts("- "))
            blocks.push_back(ui::TextBlock::bullet(line.substr(2)));
        else if (starts("Warning:") || starts("Caution:") || starts("Important:") ||
                 starts("Note:") || starts("Tip:"))
            blocks.push_back(ui::TextBlock::quote(line));
        else if (line.size() <= 60 && last != '.' && last != ':' && last != '!' && last != '?' &&
                 last != ',' && last != ';')
            blocks.push_back(ui::TextBlock::heading(line, 2));
        else
            blocks.push_back(ui::TextBlock::paragraph(line));
    }
    if (!blocks.empty() && update_offer_.notes_truncated)
        blocks.push_back(ui::TextBlock::paragraph(
            TR("update.the_rest_is_on", "The rest is on the app's page on homebrew.page.")));
    update_notes_.set_content(std::move(blocks));
    open_update_offer(feedback, false);
}

// The question. With release notes it has a third answer, What's new.
void NativeUI::Impl::open_update_offer(ui::Feedback &feedback, bool on_notes)
{
    const bool with_notes = !update_notes_.content().empty();
    ui::DialogContent content;
    content.title = TR("update.update_available", "Update available");
    char size[48] = "";
    if (update_offer_.size != 0)
        std::snprintf(size, sizeof(size), " (%.0f MB)",
                      static_cast<double>(update_offer_.size) / 1e6);
    content.body = i18n::fill(
        TR("update.is_out",
           "ProsperoAI %s is out%s.\nUpdate now downloads and checks it. ProsperoAI then "
           "closes while the new version is put in place."),
        {update_offer_.version, size});
    content.buttons.push_back({TR("update.skip", "Skip"), ui::ButtonKind::secondary, false});
    if (with_notes)
        content.buttons.push_back(
            {TR("update.what_s_new", "What's new"), ui::ButtonKind::secondary, false});
    content.buttons.push_back(
        {TR("update.update_now", "Update now"), ui::ButtonKind::primary, false});
    content.default_button =
        with_notes && on_notes ? 1 : static_cast<int>(content.buttons.size()) - 1;
    update_dialog_.open(std::move(content), feedback);
    update_ui_ = UpdateUi::offer;
}

void NativeUI::Impl::open_update_failure(const char *reason, ui::Feedback &feedback)
{
    debug::line("notice", "update: not installed: %s", reason && reason[0] ? reason : "-");
    ui::DialogContent content;
    content.title = TR("update.the_update_was_not", "The update was not installed");
    content.body = std::string(reason && reason[0]
                                   ? reason
                                   : TR("update.something_went_wrong", "Something went wrong.")) +
                   TR("update.nprosperoai_is_as_it", "\nProsperoAI is as it was.");
    content.buttons = {{TR("ui.close", "Close"), ui::ButtonKind::secondary, false},
                       {TR("update.try_again", "Try again"), ui::ButtonKind::primary, false}};
    content.default_button = 1;
    update_dialog_.open(std::move(content), feedback);
    update_ui_ = UpdateUi::failed;
}

void NativeUI::Impl::begin_update(ui::Feedback &feedback)
{
    update_progress_ = update::Progress{};
    update_ring_.set_value(0.0f, true);
    if (update::begin())
        update_ui_ = UpdateUi::working;
    else
        open_update_failure(TR("update.the_update_could_not", "The update could not start."),
                            feedback);
}

// The update's own input: while it shows, nothing under it is reached.
void NativeUI::Impl::update_modal(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    switch (update_ui_)
    {
    case UpdateUi::offer:
    case UpdateUi::failed:
    {
        // Skip, [What's new,] Update now; or Close, Try again.
        const bool offer = update_ui_ == UpdateUi::offer;
        const bool with_notes = offer && !update_notes_.content().empty();
        const ui::Event event = update_dialog_.handle(input, feedback);
        const int choice = update_dialog_.choice();
        if (event == ui::Event::activated && choice == (with_notes ? 2 : 1))
            begin_update(feedback);
        else if (event == ui::Event::activated && with_notes && choice == 1)
            update_ui_ = UpdateUi::notes;
        else if (!update_dialog_.is_open())
            update_ui_ = UpdateUi::hidden;
        break;
    }
    case UpdateUi::notes:
        if (input.is_pressed(Action::confirm))
            begin_update(feedback);
        else if (input.is_pressed(Action::back))
            open_update_offer(feedback, true);
        else
            (void)update_notes_.handle(input, feedback);
        break;
    case UpdateUi::working:
    {
        update::poll(&update_progress_);
        const update::Progress &now = update_progress_;
        update_ring_.style.mode =
            now.total != 0 ? ui::ProgressMode::determinate : ui::ProgressMode::indeterminate;
        if (now.total != 0)
            update_ring_.set_value(
                std::min(1.0f, static_cast<float>(static_cast<double>(now.done) /
                                                  static_cast<double>(now.total))));
        if (now.phase == update::Phase::ready)
        {
            if (update::apply())
            {
                update_ui_ = UpdateUi::closing;
                update_closing_age_ = 0.0f;
                feedback.play(audio::Cue::select);
            }
            else
            {
                update::finish();
                open_update_failure(
                    TR("update.the_update_helper_did", "The update helper did not answer."),
                    feedback);
            }
        }
        else if (now.phase == update::Phase::failed || now.phase == update::Phase::cancelled)
        {
            const bool cancelled = now.phase == update::Phase::cancelled;
            const std::string reason = now.error;
            update::finish();
            if (cancelled)
                update_ui_ = UpdateUi::hidden;
            else
                open_update_failure(reason.c_str(), feedback);
        }
        else if (input.is_pressed(Action::back))
        {
            update::cancel();
            feedback.play(audio::Cue::modal_close);
        }
        break;
    }
    case UpdateUi::closing:
        // Long enough to read the last line; the helper waits for the app to be gone.
        if (update_closing_age_ <= 1.5f && update_closing_age_ + dt > 1.5f)
        {
            debug::line("app", "closing for the update to %s", update_offer_.version);
            quit_ = true;
        }
        update_closing_age_ += dt;
        break;
    default:
        break;
    }
}

void NativeUI::Impl::update_tick(float dt)
{
    update_dialog_.update(dt);
    update_ring_.update(dt);
    update_notes_.update(dt);
    const float step = std::min(1.0f, dt * 14.0f);
    const bool working = update_ui_ == UpdateUi::working || update_ui_ == UpdateUi::closing;
    update_fade_ += ((working ? 1.0f : 0.0f) - update_fade_) * step;
    update_notes_fade_ +=
        ((update_ui_ == UpdateUi::notes ? 1.0f : 0.0f) - update_notes_fade_) * step;
}

// What's new: the release notes, to read before deciding.
void NativeUI::Impl::draw_update_notes(ui::Canvas &canvas) const
{
    const float shown = update_notes_fade_;
    if (shown <= 0.01f)
        return;
    gfx::DrawList &list = canvas.list;
    list.rounded_rect({0, 0, 1920, 1080}, 0, Color{0.0f, 0.0f, 0.0f, 0.55f * shown});
    list.push_opacity(shown);
    const Rect &r = kNotesPanel;
    list.rounded_rect(r, 28, palette_.page_bottom);
    panel(list, r, 28, 1);
    label(list, std::string(TR("update.prosperoai", "PROSPEROAI ")) + update_offer_.version,
          r.x + 64, r.y + 62, palette_.accent);
    text(list, TR("update.what_s_new", "What's new"), r.x + 64, r.y + 116, 40, palette_.ink, true);
    update_notes_.draw(canvas);
    // What the buttons do here, as the buttons themselves.
    auto glyphs = palette_.day ? ui::GlyphStyle::light() : ui::GlyphStyle::dark();
    glyphs.label = palette_.ink;
    const ui::Hint hints[] = {{ui::Button::cross, TR("update.update_now", "Update now")},
                              {ui::Button::circle, TR("ui.back", "Back")}};
    ui::HintLayout layout;
    layout.size = 32;
    layout.text_size = 22;
    layout.cy = r.y + r.h - 62;
    layout.item_gap = 34;
    ui::draw_hints(list, fonts_, glyphs, hints, 2, r.x + 64, false, layout);
    list.pop_opacity();
}

// The update at work: a ring, what it is doing, and how much is left.
void NativeUI::Impl::draw_update(ui::Canvas &canvas) const
{
    const float shown = update_fade_;
    if (shown <= 0.01f)
        return;
    gfx::DrawList &list = canvas.list;
    list.rounded_rect({0, 0, 1920, 1080}, 0, Color{0.0f, 0.0f, 0.0f, 0.55f * shown});
    list.push_opacity(shown);
    const Rect &r = kUpdatePanel;
    const float cx = r.x + r.w * 0.5f;
    list.rounded_rect(r, 28, palette_.page_bottom);
    panel(list, r, 28, 1);
    const bool closing = update_ui_ == UpdateUi::closing;
    const update::Progress &now = update_progress_;
    const char *doing =
        closing                                   ? TR("update.updating", "Updating")
        : now.phase == update::Phase::downloading ? TR("update.downloading", "Downloading")
        : now.phase == update::Phase::unpacking   ? TR("update.unpacking", "Unpacking")
        : now.phase == update::Phase::ready || now.phase == update::Phase::applying
            ? TR("update.finishing", "Finishing")
            : TR("screen.preparing", "Preparing");
    label(list, std::string(TR("update.prosperoai", "PROSPEROAI ")) + update_offer_.version, cx,
          r.y + 62, palette_.accent, gfx::Align::center);
    text(list, doing, cx, r.y + 122, 40, palette_.ink, true, gfx::Align::center);
    update_ring_.draw(canvas);
    char line[160];
    std::snprintf(line, sizeof(line), "%s",
                  TR("update.starting_the_update_helper", "Starting the update helper"));
    if (closing)
        std::snprintf(line, sizeof(line), "%s",
                      TR("update.prosperoai_closes_now", "ProsperoAI closes now."));
    else if (now.total != 0)
        std::snprintf(line, sizeof(line), TR("update.1f_of_1f_mb", "%.1f of %.1f MB%s%s"),
                      static_cast<double>(now.done) / 1e6, static_cast<double>(now.total) / 1e6,
                      now.time_left[0] ? "  \xC2\xB7  " : "", now.time_left);
    text(list, line, cx, r.y + 442, 26, palette_.ink, false, gfx::Align::center);
    if (closing)
    {
        text(list,
             TR("update.open_it_again_when", "Open it again when the console says it was updated."),
             cx, r.y + r.h - 54, 22, palette_.muted, false, gfx::Align::center);
    }
    else
    {
        // Nothing is changed before the download is checked; the way out is its button.
        auto glyphs = palette_.day ? ui::GlyphStyle::light() : ui::GlyphStyle::dark();
        glyphs.label = palette_.muted;
        const ui::Hint hints[] = {
            {ui::Button::circle, TR("update.cancel_nothing_is_changed",
                                    "Cancel. Nothing is changed until the download is checked.")}};
        ui::HintLayout layout;
        layout.size = 30;
        layout.text_size = 22;
        layout.cy = r.y + r.h - 62;
        const float width = ui::button_width(ui::Button::circle, 30) + 12 +
                            fonts_.regular.font->measure(hints[0].label, 22);
        ui::draw_hints(list, fonts_, glyphs, hints, 1, cx - width * 0.5f, false, layout);
    }
    list.pop_opacity();
}
} // namespace prospero
