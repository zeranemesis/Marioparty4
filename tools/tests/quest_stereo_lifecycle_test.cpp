// Runs on an Android device without starting OpenXR. Exercise the real lease
// methods, so empty-camera transitions cannot leak or steal GPU-owned images.
#include "stereo_view.hpp"
#include "../../include/port/quest_scene_fit.hpp"
#include "../../extern/aurora/lib/surface_lifecycle.hpp"
#include <cassert>
#include <cstdio>
#include <unistd.h>

namespace quest {
struct StereoViewTestAccess {
  static void presented(StereoView& view, uint64_t tag) { view.mPresentedTag = tag; }
  static int completed(StereoView& view) {
    auto* slot = view.newest_completed();
    return slot ? static_cast<int>(slot - view.mSlots.data()) : -1;
  }
  static void enable(StereoView& view) { view.mEnabled = true; }
  static void copying(StereoView& view, uint32_t image) {
    view.mSlots[image].state = StereoView::State::Copying;
  }
  static void copied(StereoView& view, uint32_t image) { view.release_slot(view.mSlots[image]); }
  static bool board(const StereoView& view, uint32_t image) { return view.mSlots[image].board; }

};
}

int main() {
  aurora::window::SurfaceLifecycle surface(true);
  const auto previousSurface = surface.generation();
  // Reproduce destroy/create occurring entirely between rendered frames.
  surface.set_ready(false);
  surface.set_ready(true);
  assert(surface.ready() && surface.generation() != previousSurface);
  const auto replacement = surface.generation();
  surface.set_ready(true);
  assert(surface.generation() == replacement);
  surface.set_ready(false);
  assert(!surface.ready() && surface.generation() != replacement);
  float scale = 0.0f;
  assert(partyboard::quest::scene_scale(1000.0f, 90.0f, 1.4f, scale));
  assert(std::abs(scale - 1.0f) < 0.0001f);
  assert(partyboard::quest::scene_scale(10.0f, 90.0f, 1.0f, scale) && scale == 16.0f);
  assert(partyboard::quest::scene_scale(100000.0f, 90.0f, 1.0f, scale) && scale == 0.25f);
  assert(!partyboard::quest::scene_scale(0.0f, 90.0f, 1.0f, scale));
  assert(!partyboard::quest::scene_scale(100.0f, 180.0f, 1.0f, scale));
  assert(!partyboard::quest::scene_scale(100.0f, 90.0f, NAN, scale));
  float clip[2][16]{};
  for (int eye = 0; eye < 2; ++eye) for (int d = 0; d < 4; ++d) clip[eye][d*5] = 1;
  const float visible[3]{0,0,-0.5f}, hidden[3]{2,0,-0.5f}, edge[3]{1.05f,0,-0.5f};
  assert(partyboard::quest::sphere_visible(clip, visible, 0.1f));
  assert(!partyboard::quest::sphere_visible(clip, hidden, 0.1f));
  assert(partyboard::quest::sphere_visible(clip, edge, 0.1f));
  clip[1][3] = -2; // visible to the second eye only
  assert(partyboard::quest::sphere_visible(clip, hidden, 0.1f));
  assert(partyboard::quest::sphere_visible(clip, hidden, NAN));
  float center[3]{};
  const float minimum[3]{1000, -500, -4000}, maximum[3]{6600, 300, -1200};
  assert(partyboard::quest::board_fit(minimum, maximum, scale, center));
  assert(scale == 0.5f && center[0] == 3800 && center[1] == -500 && center[2] == -2600);
  // Lowest board surface maps to zero; camera target changes cannot affect this fit.
  assert((minimum[1] - center[1]) * scale == 0);
  const float invalid[3]{NAN, 0, 0};
  assert(!partyboard::quest::board_fit(invalid, maximum, scale, center));
  assert(!partyboard::quest::board_fit(maximum, minimum, scale, center));
  assert(!partyboard::quest::board_fit(minimum, minimum, scale, center));
  {
    quest::StereoView transfer;
    quest::StereoViewTestAccess::enable(transfer);
    quest::StereoFrame oldFrame{}, nextFrame{};
    assert(transfer.game_frame(oldFrame));
    assert(transfer.game_frame(nextFrame));
    int pending[2];
    assert(pipe(pending) == 0);
    transfer.submitted(nextFrame.image, nextFrame.tag, pending[0]);
    assert(quest::StereoViewTestAccess::completed(transfer) == -1);
    transfer.submitted(oldFrame.image, oldFrame.tag, -1);
    assert(quest::StereoViewTestAccess::completed(transfer) == static_cast<int>(oldFrame.image));
    const char signal = 1;
    assert(write(pending[1], &signal, 1) == 1);
    assert(quest::StereoViewTestAccess::completed(transfer) == static_cast<int>(nextFrame.image));
    // A late completed source cannot replace a newer image already shown.
    quest::StereoViewTestAccess::presented(transfer, nextFrame.tag);
    assert(quest::StereoViewTestAccess::completed(transfer) == -1);
    close(pending[1]);
    quest::StereoViewTestAccess::copied(transfer, oldFrame.image);
    quest::StereoViewTestAccess::copied(transfer, nextFrame.image);
  }
  quest::StereoView view;
  // A separately composited HUD must not show stale menus before a world.
  assert(view.hud_layer(XR_NULL_HANDLE, nullptr) == nullptr);
  quest::StereoViewTestAccess::enable(view);
  quest::StereoFrame frame{};
  view.set_screen_required(true);
  assert(view.hud_layer(XR_NULL_HANDLE, nullptr) == nullptr);
  assert(view.screen_required() && !view.game_frame(frame) && !view.world_only());
  view.set_screen_required(false);
  assert(!view.screen_required() && view.world_only());
  // The former age-based cleanup stalled after three empty camera frames.
  for (int i = 0; i < 100; ++i) {
    assert(view.game_frame(frame));
    view.cancelled(frame.image, frame.tag);
  }
  quest::StereoFrame queued[3]{};
  view.set_board_mode(true);
  assert(view.game_frame(frame));
  assert(quest::StereoViewTestAccess::board(view, frame.image));
  view.set_board_mode(false);
  // Pending board frames keep their presentation policy across scene changes.
  assert(quest::StereoViewTestAccess::board(view, frame.image));
  view.cancelled(frame.image, frame.tag);
  for (auto& item : queued) {
    assert(view.game_frame(item));
    assert(!quest::StereoViewTestAccess::board(view, item.image));
  }
  // Frames held for asynchronous rendering must never be reclaimed by age.
  for (int i = 0; i < 100; ++i) {
    assert(!view.game_frame(frame));
  }
  view.cancelled(queued[0].image, queued[0].tag + 1);
  assert(!view.game_frame(frame));
  view.cancelled(queued[0].image, queued[0].tag);
  assert(view.game_frame(frame));
  assert(frame.image == queued[0].image && frame.tag != queued[0].tag);
  // A late cancellation from a previous use cannot cancel the new lease.
  view.cancelled(queued[0].image, queued[0].tag);
  quest::StereoFrame spare{};
  assert(!view.game_frame(spare));
  view.submitted(frame.image, frame.tag, -1);
  // Submission transfers ownership to the compositor, not to cancellation.
  view.cancelled(frame.image, frame.tag);
  assert(!view.game_frame(spare));
  // A source still read by the copy GPU is neither free nor cancellable.
  quest::StereoViewTestAccess::copying(view, frame.image);
  view.cancelled(frame.image, frame.tag);
  assert(!view.game_frame(spare));
  view.submitted(frame.image, frame.tag, -1); // a stale callback cannot take it back
  assert(!view.game_frame(spare));
  quest::StereoViewTestAccess::copied(view, frame.image);
  assert(view.game_frame(spare));
  assert(spare.tag != frame.tag);
  std::puts("PASS: rapid surface replacement, scene framing, empty cameras, ring exhaustion, GPU ownership and stale callbacks");
}
