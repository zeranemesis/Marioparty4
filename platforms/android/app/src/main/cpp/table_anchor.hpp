#pragma once

// Where the game sits in the room, kept between sessions.
//
// The place is a spatial anchor (XR_FB_spatial_entity), which the headset
// keeps fixed to the real room even when its tracking re-maps the play area.
// Its pose in STAGE space is saved too and used while the anchor loads, or
// without one (anchor extensions missing, anchor lost).

#include "xr_util.hpp"

#include <cstdint>
#include <string>

namespace quest {

// Saved with the anchor: what the player chose in placement mode.
struct TableSettings {
  bool placed = false;
  // A point on the table, turned so +Z faces the player.
  XrPosef pose = identity_pose();
  float screenWidth = 1.0f;     // meters
  int resolution = 1080;        // screen height in pixels: 1080, 1440 or 2160
  bool passthrough = true;      // the room around the game (mixed reality)
  bool diorama = true;          // the game's world as a model on the table
  float modelScale = 0.00025f;  // the model: meters per game unit
  std::string anchorUuid;       // hex; empty without an anchor

  bool load(const std::string& path);
  void save(const std::string& path) const;
};

class TableAnchor {
public:
  // The instance extensions anchors need; without them, only the saved pose is used.
  static const char* const kExtensions[3];

  void init(XrInstance instance, XrSession session, XrSpace stage);
  void destroy();

  // Starts loading the anchor saved as `uuid` (hex).
  void load(const std::string& uuid);
  // Replaces the anchor by a new one at `pose` (STAGE space), saved on the headset.
  void place(const XrPosef& pose, XrTime time);
  // Anchor events; returns true when `event` was one of them.
  bool handle_event(const XrEventDataBuffer& event);
  // The anchor's pose in STAGE space, when the headset tracks it.
  bool locate(XrTime time, XrPosef& pose) const;
  // The uuid of an anchor saved since the last call, to keep with the settings.
  bool take_saved_uuid(std::string& uuid);

private:
  void enable_component(XrSpace space, XrSpaceComponentTypeFB component);
  void save(XrSpace space);
  void erase(XrSpace space);

  XrInstance mInstance = XR_NULL_HANDLE;
  XrSession mSession = XR_NULL_HANDLE;
  XrSpace mStage = XR_NULL_HANDLE;
  XrSpace mSpace = XR_NULL_HANDLE;   // the current anchor, once created or loaded
  XrSpace mErasing = XR_NULL_HANDLE; // the previous one, destroyed once erased
  bool mReady = false;               // mSpace is locatable
  std::string mSavedUuid;
  bool mSavedPending = false;
  XrAsyncRequestIdFB mCreateRequest = 0;
  XrAsyncRequestIdFB mQueryRequest = 0;

  PFN_xrCreateSpatialAnchorFB mCreate = nullptr;
  PFN_xrSetSpaceComponentStatusFB mSetStatus = nullptr;
  PFN_xrGetSpaceComponentStatusFB mGetStatus = nullptr;
  PFN_xrSaveSpaceFB mSave = nullptr;
  PFN_xrEraseSpaceFB mErase = nullptr;
  PFN_xrQuerySpacesFB mQuery = nullptr;
  PFN_xrRetrieveSpaceQueryResultsFB mRetrieve = nullptr;
};

} // namespace quest
