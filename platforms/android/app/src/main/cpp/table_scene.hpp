#pragma once

// The tables of the headset's room scan (XR_FB_scene): Space Setup on Quest 3
// finds them, on Quest 2 the player draws them. The table calibration
// (quest_xr.cpp) takes the height and size of the one the controller lies on.
//
// The scan is the player's data: Horizon OS asks for the "spatial data"
// permission (com.oculus.permission.USE_SCENE, QuestVr.java) before any query
// returns it. Without the extensions or the permission, there are no tables
// and the calibration measures with the controller alone.

#include "table_fit.hpp"
#include "xr_util.hpp"

#include <vector>

namespace quest {

class TableScene {
public:
  // What the calibration panel says about the scan (QuestVr.drawHelp has the same values).
  enum class State : int {
    Unavailable = 0, // no scene extension on this headset
    NoPermission = 1,
    Searching = 2,
    NoTable = 3,
    Found = 4,
    Capturing = 5, // Space Setup is open
  };

  // The instance extension scenes need, on top of TableAnchor's.
  static const char* const kExtension;
  static const char* const kCaptureExtension;

  void init(XrInstance instance, XrSession session, XrSpace stage, bool capture);
  void destroy();

  // The player allowed (or refused) the spatial data permission: queries now
  // find the scan, or stop trying.
  void set_permission(bool granted);
  // Looks for the scan's tables again (async: handle_event takes the results).
  void query();
  // Opens Space Setup to scan the room; the tables are queried again after.
  bool request_capture();
  // Scene events; true when `event` was one of this class's requests.
  bool handle_event(const XrEventDataBuffer& event);

  // The tables, located now in STAGE space.
  std::vector<ScannedTable> tables(XrTime time) const;
  State state() const;
  // The largest table's size (meters), for the panel; zero without one.
  void largest(float& length, float& width) const;
  bool available() const { return mQuery != nullptr; }
  bool can_capture() const { return mCapture != nullptr && mPermission; }

private:
  struct Found {
    XrSpace space = XR_NULL_HANDLE;
    XrRect2Df bounds{};
  };
  void add(XrSpace space);
  void clear();

  XrInstance mInstance = XR_NULL_HANDLE;
  XrSession mSession = XR_NULL_HANDLE;
  XrSpace mStage = XR_NULL_HANDLE;
  bool mPermission = false;
  bool mQueried = false;   // a query ended since the permission or last capture
  bool mCapturing = false;
  XrAsyncRequestIdFB mQueryRequest = 0;
  XrAsyncRequestIdFB mCaptureRequest = 0;
  std::vector<Found> mTables;
  std::vector<Found> mPending; // this query's tables, until it completes

  PFN_xrQuerySpacesFB mQuery = nullptr;
  PFN_xrRetrieveSpaceQueryResultsFB mRetrieve = nullptr;
  PFN_xrGetSpaceSemanticLabelsFB mLabels = nullptr;
  PFN_xrGetSpaceBoundingBox2DFB mBounds = nullptr;
  PFN_xrGetSpaceComponentStatusFB mGetStatus = nullptr;
  PFN_xrSetSpaceComponentStatusFB mSetStatus = nullptr;
  PFN_xrRequestSceneCaptureFB mCapture = nullptr;
};

} // namespace quest
