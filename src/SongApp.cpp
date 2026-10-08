/**
 * @file SongApp.cpp
 * @brief Song -- the kit's scenes arranged on a timeline.
 *
 * A song is a row of blocks, each a scene played a number of times in a row:
 * "Seq 1 x4, Seq 2 x4, Seq 1 x2, Seq 3 x1". It is drawn the way a linear DAW
 * draws an arrangement: a bar ruler, and every block as wide as it plays --
 * repeats times the scene's own bar count -- in its scene's colour, with a
 * playhead running across while it plays.
 *
 * The arrangement lives in the sequencer (crosspad-core) and is saved with the
 * kit's sequences; this app only draws and edits it. PLAY on the transport bar
 * is song mode while this screen holds the transport: the sequencer walks the
 * blocks, changing scene on the bar line (UX 2000 section 6, screen 3).
 *
 * The encoder edits the selected block: turn = its scene, click = its repeats,
 * click again = the next block. A tap on a block selects it.
 *
 * The pads stay the Sampler's: no pad logic of its own, so they play the kit
 * while the song runs.
 */

#include "crosspad-song/SongApp.hpp"

// `class App` lives in a different header per platform, but both derive from
// crosspad_gui::AppBase<App>, so the lifecycle calls used here are identical.
#if __has_include("pc_stubs/PcApp.hpp")
  #include "pc_stubs/PcApp.hpp"
#else
  #include "crosspad-gui/app/App.hpp"
#endif

#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/SequencerController.hpp"
#include "crosspad/transport/BlockSongTransport.hpp"
#include "crosspad-gui/components/transport_bar.h"
#include "crosspad-gui/widgets/gui_helpers.h"

#include "lvgl.h"

#include <cstdarg>
#include <cstdio>

// Encoder navigation is an embedded-only concern -- the simulator drives the UI
// with a pointer. Snapshot whichever group was active before the app opened and
// restore that, rather than a global launcher group that goes stale whenever the
// launcher is rebuilt.
#ifdef ESP_PLATFORM
extern "C" {
  extern lv_indev_t* indev_encoderTop;
}
#define SONG_USE_ENCODER 1
#endif

namespace {

/* ── set_text_fmt() is not used here, and this is why ─────────────
 *
 * It frees the label's old text and allocates the new one, but it does not
 * revert the dots of a truncated label first -- lv_label_set_text() does
 * exactly that, one line in, "in case text == label->text". So when a long
 * line is replaced by a shorter one, LVGL still holds the offset the dots were
 * written at, and on the next refresh it puts the four saved characters back at
 * that offset -- past the end of the new, shorter buffer. The heap block next
 * door loses its tail canary, and the crash lands minutes later on whatever
 * frees that block: an assert deep inside LVGL, with nothing in the backtrace
 * to say who wrote the bytes. It read as "Arrange crashes when I click a
 * block", because this screen is the one that formats a line of changing length
 * on every click.
 *
 * Formatting it ourselves and handing lv_label_set_text() a finished string is
 * both the safe way and what every other screen in this firmware does. */
static void set_text_fmt(lv_obj_t* label, const char* fmt, ...)
{
    if (!label || !fmt) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(label, buf);
}

constexpr int32_t  kPxPerBar  = 12;
constexpr int32_t  kRulerH    = 12;
constexpr int32_t  kBlockH    = 58;
constexpr int32_t  kBlockGap  = 2;
constexpr uint32_t kTickMs    = 50;
constexpr uint8_t  kMaxBlocks = 32;

/* A screen y as a y inside the root, which begins under the transport bar:
 * the screen is laid out on the template's bands (context row at 34, content
 * from 60, soft keys at 206). */
#define SONG_Y(v) ((v) - TRANSPORT_BAR_HEIGHT)

const lv_color_t kBg      = lv_color_hex(0x16181B);
const lv_color_t kPanel   = lv_color_hex(0x20242A);
const lv_color_t kWell    = lv_color_hex(0x1B1E22);
const lv_color_t kInk     = lv_color_hex(0x16181B);
const lv_color_t kText    = lv_color_hex(0xE8EAED);
const lv_color_t kDim     = lv_color_hex(0x7B828B);
const lv_color_t kAccent  = lv_color_hex(0xC6FF47);
const lv_color_t kHead    = lv_color_hex(0xFFFFFF);
const lv_color_t kMint    = lv_color_hex(0x00FF88);

/// A scene's colour, the same for every block of it, so a repeat of Seq 1
/// reads as Seq 1 at a glance anywhere on the timeline.
const uint32_t kSceneColors[] = {
    0xC6FF47, 0x4EA6D3, 0xFFC857, 0xFF7A9A, 0xB48CFF, 0xFF9552, 0x4FE0B0, 0x7AA2FF,
};

/// What the encoder turns on the selected block.
enum class Part : uint8_t { Scene, Repeats };

lv_obj_t* s_root      = nullptr;
lv_obj_t* s_timeline  = nullptr;   // the field the encoder edits; scrolls horizontally
lv_obj_t* s_playhead  = nullptr;
lv_obj_t* s_posLabel  = nullptr;
lv_obj_t* s_infoLabel = nullptr;
lv_obj_t* s_loopBtn   = nullptr;
lv_obj_t* s_blocks[kMaxBlocks] = {};
lv_obj_t* s_keys[3] = {};
lv_timer_t* s_timer   = nullptr;

uint8_t  s_sel = 0;
Part     s_part = Part::Scene;
uint32_t s_shape = 0;      // what the timeline was last built from
int      s_playing = -1;   // block drawn as playing, -1 none

#ifdef SONG_USE_ENCODER
lv_group_t* s_encGroup = nullptr;
lv_group_t* s_prevEncGroup = nullptr;
#endif

crosspad::SequencerController* seq()
{
    return crosspad::getPlatformServices().sequencer;
}

struct Block { uint8_t scene = 0, repeats = 1, bars = 1; };

bool block_at(uint8_t i, Block& b)
{
    auto* sq = seq();
    if (!sq || !sq->getSongBlock(i, b.scene, b.repeats)) return false;
    b.bars = sq->getSceneBars(b.scene);
    if (!b.bars) b.bars = 1;
    return true;
}

int32_t block_bars(const Block& b) { return (int32_t)b.repeats * b.bars; }

int32_t song_bars()
{
    auto* sq = seq();
    int32_t total = 0;
    for (uint8_t i = 0; sq && i < sq->getSongLength(); ++i) { Block b; block_at(i, b); total += block_bars(b); }
    return total;
}

/// Everything the drawing depends on, folded into one word: when it changes
/// -- an edit here, a kit load, a scene's length changed in the Sequencer --
/// the timeline is rebuilt.
uint32_t song_shape()
{
    auto* sq = seq();
    if (!sq) return 0;
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
    mix(sq->getSongLength());
    mix(sq->getSceneCount());
    for (uint8_t i = 0; i < sq->getSongLength(); ++i) {
        Block b;
        block_at(i, b);
        mix(((uint32_t)b.scene << 16) | ((uint32_t)b.repeats << 8) | b.bars);
    }
    return h;
}

void update_info()
{
    auto* sq = seq();
    if (!sq || !s_infoLabel) return;
    Block b;
    if (sq->getSongLength() && block_at(s_sel, b)) {
        set_text_fmt(s_infoLabel, "Block %u  -  Seq %u x%u  -  %d bars",
                     (unsigned)(s_sel + 1), (unsigned)(b.scene + 1), (unsigned)b.repeats,
                     (int)block_bars(b));
    } else {
        lv_label_set_text(s_infoLabel, "Empty song -- + ADD puts the first block in");
    }
}

void paint_block(uint8_t i)
{
    lv_obj_t* o = s_blocks[i];
    if (!o) return;
    const bool sel = i == s_sel;
    const bool playing = (int)i == s_playing;
    lv_obj_set_style_border_width(o, sel ? 2 : 0, 0);
    lv_obj_set_style_border_color(o, kHead, 0);
    lv_obj_set_style_bg_opa(o, playing ? LV_OPA_COVER : LV_OPA_80, 0);
}

void label_block(uint8_t i)
{
    lv_obj_t* o = s_blocks[i];
    Block b;
    if (!o || !block_at(i, b)) return;
    lv_obj_t* l = lv_obj_get_child(o, 0);
    if (!l) return;
    /* Which of its two numbers the encoder turns: the other one is dimmed. */
    const bool sel = i == s_sel;
    lv_label_set_recolor(l, true);
    if (!sel)                   set_text_fmt(l, "S%u\nx%u", (unsigned)(b.scene + 1), (unsigned)b.repeats);
    else if (s_part == Part::Scene) set_text_fmt(l, "S%u\n#444444 x%u#", (unsigned)(b.scene + 1), (unsigned)b.repeats);
    else                        set_text_fmt(l, "#444444 S%u#\nx%u", (unsigned)(b.scene + 1), (unsigned)b.repeats);
}

void select_block(uint8_t i, Part part)
{
    auto* sq = seq();
    const uint8_t n = sq ? sq->getSongLength() : 0;
    if (!n) { s_sel = 0; update_info(); return; }
    if (i >= n) i = (uint8_t)(n - 1);
    const uint8_t prev = s_sel;
    s_sel = i;
    s_part = part;
    /* Choosing a block is choosing where PLAY on the bar starts. */
    if (auto* t = crosspad::blockSongTransport()) t->setStartBlock(i);
    if (prev < n) { paint_block(prev); label_block(prev); }
    paint_block(i);
    label_block(i);
    if (s_blocks[i]) lv_obj_scroll_to_view(s_blocks[i], LV_ANIM_ON);
    update_info();
}

void block_clicked_cb(lv_event_t* e)
{
    select_block((uint8_t)(uintptr_t)lv_event_get_user_data(e), Part::Scene);
}

void rebuild_timeline()
{
    auto* sq = seq();
    if (!s_timeline || !sq) return;

    lv_obj_clean(s_timeline);
    for (auto& b : s_blocks) b = nullptr;
    s_playhead = nullptr;
    s_playing = -1;

    const uint8_t n = sq->getSongLength();
    int32_t x = 0;
    for (uint8_t i = 0; i < n && i < kMaxBlocks; ++i) {
        Block b;
        if (!block_at(i, b)) break;
        const int32_t w = block_bars(b) * kPxPerBar;

        lv_obj_t* o = lv_obj_create(s_timeline);
        lv_obj_remove_style_all(o);
        lv_obj_set_pos(o, x, kRulerH + 2);
        lv_obj_set_size(o, w - kBlockGap, kBlockH);
        lv_obj_set_style_radius(o, 4, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(kSceneColors[b.scene % 8]), 0);
        lv_obj_set_style_pad_all(o, 3, 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(o, block_clicked_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        /* Scene over repeats, top-left, clipped when the block is short. */
        lv_obj_t* l = lv_label_create(o);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(l, kInk, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(l, LV_PCT(100));

        /* A tick per pass, so x4 looks like four of something. */
        for (uint8_t r = 1; r < b.repeats; ++r) {
            lv_obj_t* t = lv_obj_create(o);
            lv_obj_remove_style_all(t);
            lv_obj_set_size(t, 1, 10);
            lv_obj_set_style_bg_color(t, kInk, 0);
            lv_obj_set_style_bg_opa(t, LV_OPA_40, 0);
            lv_obj_align(t, LV_ALIGN_BOTTOM_LEFT, r * b.bars * kPxPerBar - 3, 3);
        }

        s_blocks[i] = o;
        x += w;
    }

    /* Ruler: a number every four bars, a tick every bar, across the song and
     * a little past it so the end is visible as an end. */
    const int32_t bars = x / kPxPerBar + 8;
    for (int32_t bar = 0; bar < bars; ++bar) {
        lv_obj_t* t = lv_obj_create(s_timeline);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, 1, bar % 4 ? 3 : 6);
        lv_obj_set_style_bg_color(t, kDim, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_pos(t, bar * kPxPerBar, kRulerH - (bar % 4 ? 3 : 6));
        lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
        if (bar % 4 == 0) {
            lv_obj_t* num = lv_label_create(s_timeline);
            lv_obj_set_style_text_font(num, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(num, kDim, 0);
            set_text_fmt(num, "%d", (int)(bar + 1));
            lv_obj_set_pos(num, bar * kPxPerBar + 2, -2);
        }
    }

    s_playhead = lv_obj_create(s_timeline);
    lv_obj_remove_style_all(s_playhead);
    lv_obj_set_size(s_playhead, 2, kRulerH + kBlockH + 4);
    lv_obj_set_style_bg_color(s_playhead, kMint, 0);
    lv_obj_set_style_bg_opa(s_playhead, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_playhead, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_playhead, LV_OBJ_FLAG_CLICKABLE);

    s_shape = song_shape();
    for (uint8_t i = 0; i < kMaxBlocks; ++i) { paint_block(i); label_block(i); }
    select_block(s_sel, s_part);
}

void paint_loop()
{
    auto* sq = seq();
    if (!sq || !s_loopBtn) return;
    const bool loop = sq->getSongLoop();
    lv_obj_set_style_bg_color(s_loopBtn, loop ? kAccent : kPanel, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_loopBtn, 0), loop ? kInk : kDim, 0);
}

void tick_cb(lv_timer_t*)
{
    auto* sq = seq();
    if (!sq) return;
    if (song_shape() != s_shape) rebuild_timeline();
    paint_loop();

    const bool playing = sq->getState() == crosspad::SequencerState::Running && sq->getSongMode();
    const int now = playing ? sq->getSongPosition() : -1;
    if (now != s_playing) {
        const int was = s_playing;
        s_playing = now;
        if (was >= 0 && was < kMaxBlocks) paint_block((uint8_t)was);
        if (now >= 0 && now < kMaxBlocks) paint_block((uint8_t)now);
    }

    int32_t at = 0;
    for (uint8_t i = 0; i < sq->getSongLength(); ++i) {
        Block b;
        block_at(i, b);
        if ((int)i < now) at += block_bars(b);
        if ((int)i == now) {
            const float into = (float)sq->getSongPass() * b.bars + sq->getProgress() * b.bars;
            at = at * kPxPerBar + (int32_t)(into * kPxPerBar);
        }
    }
    const int32_t total = song_bars();

    if (!s_playhead) return;
    if (!playing) {
        lv_obj_add_flag(s_playhead, LV_OBJ_FLAG_HIDDEN);
        set_text_fmt(s_posLabel, "%d bars", (int)total);
        return;
    }
    lv_obj_remove_flag(s_playhead, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_playhead, at, 0);
    set_text_fmt(s_posLabel, "bar %d / %d", (int)(at / kPxPerBar + 1), (int)total);

    /* Follow the playhead: once it passes two thirds of the view, page on. */
    const int32_t view = lv_obj_get_content_width(s_timeline);
    const int32_t left = lv_obj_get_scroll_x(s_timeline);
    if (at < left || at > left + view * 2 / 3) {
        lv_obj_scroll_to_x(s_timeline, at > view / 3 ? at - view / 3 : 0, LV_ANIM_ON);
    }
}

// ── actions ────────────────────────────────────────────────────────────────

void loop_cb(lv_event_t*)
{
    if (auto* sq = seq()) sq->setSongLoop(!sq->getSongLoop());
    paint_loop();
}

void edit_selected(int dScene, int dRepeats)
{
    auto* sq = seq();
    Block b;
    if (!sq || !block_at(s_sel, b)) return;
    const int scenes = sq->getSceneCount() ? sq->getSceneCount() : 1;
    const int scene = ((int)b.scene + dScene + scenes) % scenes;
    int reps = (int)b.repeats + dRepeats;
    if (reps < 1) reps = 1;
    if (reps > 16) reps = 16;
    sq->songSet(s_sel, (uint8_t)scene, (uint8_t)reps);
    rebuild_timeline();
}

/// The timeline is the field: a click puts it in edit mode, where turning
/// changes the selected block's scene or repeats and a click moves from scene
/// to repeats and on to the next block.
void timeline_cb(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY) {
        const uint32_t key = lv_event_get_key(e);
        const int d = (key == LV_KEY_RIGHT || key == LV_KEY_UP) ? 1
                    : (key == LV_KEY_LEFT || key == LV_KEY_DOWN) ? -1 : 0;
        if (!d) return;
        if (s_part == Part::Scene) edit_selected(d, 0);
        else                       edit_selected(0, d);
        return;
    }
    if (code == LV_EVENT_CLICKED) {
        lv_indev_t* in = lv_indev_active();
        if (!in || lv_indev_get_type(in) != LV_INDEV_TYPE_ENCODER) return;
        auto* sq = seq();
        const uint8_t n = sq ? sq->getSongLength() : 0;
        if (!n) return;
        if (s_part == Part::Scene) select_block(s_sel, Part::Repeats);
        else                       select_block((uint8_t)((s_sel + 1) % n), Part::Scene);
    }
}

/// After the selected block, the same scene once: the next part of a song is
/// most often more of the same, and it is one press to change.
void add_cb(lv_event_t*)
{
    auto* sq = seq();
    if (!sq) return;
    const uint8_t n = sq->getSongLength();
    Block b;
    const uint8_t scene = (n && block_at(s_sel, b)) ? b.scene : 0;
    const uint8_t at = n ? (uint8_t)(s_sel + 1) : 0;
    if (sq->songInsert(at, scene, 1)) s_sel = at;
    s_part = Part::Scene;
    rebuild_timeline();
}

/// A copy of the selected block, straight after it.
void dup_cb(lv_event_t*)
{
    auto* sq = seq();
    Block b;
    if (!sq || !block_at(s_sel, b)) return;
    if (sq->songInsert((uint8_t)(s_sel + 1), b.scene, b.repeats)) s_sel = (uint8_t)(s_sel + 1);
    rebuild_timeline();
}

void del_cb(lv_event_t*)
{
    auto* sq = seq();
    if (!sq || !sq->getSongLength()) return;
    sq->songRemove(s_sel);
    if (s_sel && s_sel >= sq->getSongLength()) --s_sel;
    rebuild_timeline();
}

lv_obj_t* make_button(lv_obj_t* parent, int32_t w, int32_t h, const lv_font_t* font,
                      const char* text, lv_event_cb_t cb)
{
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, kPanel, 0);
    lv_obj_set_style_outline_color(b, kHead, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, kText, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t col, const char* text)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_label_set_text(l, text);
    return l;
}

} // namespace

lv_obj_t* Song_create(lv_obj_t* parent, App* app)
{
#ifdef SONG_USE_ENCODER
    if (indev_encoderTop) s_prevEncGroup = lv_indev_get_group(indev_encoderTop);
    if (!s_encGroup) s_encGroup = lv_group_create();
    if (indev_encoderTop) lv_indev_set_group(indev_encoderTop, s_encGroup);
#endif

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, kBg, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // -- context row: what this is, how long, LOOP --
    lv_obj_t* title = make_label(s_root, &lv_font_montserrat_16, kText, "Song");
    lv_obj_set_pos(title, 8, SONG_Y(36));
    s_posLabel = make_label(s_root, &lv_font_montserrat_10, kDim, "");
    lv_obj_set_pos(s_posLabel, 66, SONG_Y(41));
    s_loopBtn = make_button(s_root, 56, 22, &lv_font_montserrat_14, "LOOP", loop_cb);
    lv_obj_set_pos(s_loopBtn, 258, SONG_Y(34));

    // -- the timeline: the field the encoder edits --
    s_timeline = lv_CreateEditableButton(s_root);
    lv_obj_remove_style_all(s_timeline);
    lv_obj_set_pos(s_timeline, 6, SONG_Y(60));
    lv_obj_set_size(s_timeline, 308, kRulerH + kBlockH + 8);
    lv_obj_set_style_bg_color(s_timeline, kWell, 0);
    lv_obj_set_style_bg_opa(s_timeline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_timeline, 6, 0);
    lv_obj_set_style_pad_hor(s_timeline, 4, 0);
    lv_obj_set_style_pad_top(s_timeline, 2, 0);
    lv_obj_set_style_outline_color(s_timeline, kHead, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(s_timeline, 1, LV_STATE_FOCUS_KEY);
    lv_obj_add_flag(s_timeline, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_timeline, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_timeline, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_timeline, timeline_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_timeline, timeline_cb, LV_EVENT_CLICKED, nullptr);

    s_infoLabel = make_label(s_root, &lv_font_montserrat_10, kDim, "");
    lv_obj_set_pos(s_infoLabel, 8, SONG_Y(148));
    lv_obj_set_width(s_infoLabel, 304);
    lv_label_set_long_mode(s_infoLabel, LV_LABEL_LONG_DOT);
    lv_obj_t* hint = make_label(s_root, &lv_font_montserrat_10, kAccent,
                                "turn = sequence  -  click = repeats");
    lv_obj_set_pos(hint, 8, SONG_Y(164));

    // -- soft keys: on the selected block --
    static const struct { const char* text; lv_event_cb_t cb; } kKeys[3] = {
        { "+ ADD", add_cb }, { "DUP", dup_cb }, { "DEL", del_cb },
    };
    constexpr int32_t kKeyW = (308 - 2 * 4) / 3;
    for (int i = 0; i < 3; ++i) {
        s_keys[i] = make_button(s_root, kKeyW, 30, &lv_font_montserrat_14, kKeys[i].text, kKeys[i].cb);
        lv_obj_set_pos(s_keys[i], 6 + i * (kKeyW + 4), SONG_Y(206));
    }

    lv_obj_update_layout(s_root);
    rebuild_timeline();
    paint_loop();

#ifdef SONG_USE_ENCODER
    if (s_encGroup) {
        lv_group_add_obj(s_encGroup, s_timeline);
        for (auto* k : s_keys) lv_group_add_obj(s_encGroup, k);
        lv_group_add_obj(s_encGroup, s_loopBtn);
        lv_group_focus_obj(s_timeline);
    }
#endif

    s_timer = lv_timer_create(tick_cb, kTickMs, nullptr);

    if (app) {
        app->setOnShow([](lv_obj_t*) {
            /* PLAY on the bar plays this song while this screen is up, and
             * after it: a claim outlives its screen. */
            crosspad::transportClaim(crosspad::blockSongTransport());
            crosspad_gui::transportbar_refresh();
            if (s_timer) lv_timer_resume(s_timer);
#ifdef SONG_USE_ENCODER
            if (indev_encoderTop && s_encGroup) lv_indev_set_group(indev_encoderTop, s_encGroup);
#endif
        });
        app->setOnHide([](lv_obj_t*) {
            if (s_timer) lv_timer_pause(s_timer);
#ifdef SONG_USE_ENCODER
            if (indev_encoderTop && s_prevEncGroup) lv_indev_restore_group(indev_encoderTop, s_prevEncGroup);
#endif
        });
    }

    return s_root;
}

void Song_destroy(lv_obj_t* root)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    for (auto& b : s_blocks) b = nullptr;
    for (auto& k : s_keys) k = nullptr;
    s_root = s_timeline = s_playhead = s_posLabel = s_infoLabel = nullptr;
    s_loopBtn = nullptr;
    s_playing = -1;

#ifdef SONG_USE_ENCODER
    // Hand navigation back before the group dies, or the screen behind comes
    // back unnavigable and only a reboot fixes it.
    if (indev_encoderTop && s_prevEncGroup) lv_indev_restore_group(indev_encoderTop, s_prevEncGroup);
    if (s_encGroup) {
        lv_group_delete(s_encGroup);
        s_encGroup = nullptr;
    }
    s_prevEncGroup = nullptr;
#endif

    lv_obj_delete_async(root);
}

// Auto-registration: no pad logic -- the pads stay the Sampler's and play
// while the song runs.
REGISTER_APP(Song, nullptr, "song.png",
             Song_create, Song_destroy,
             nullptr, nullptr, nullptr, 0)
