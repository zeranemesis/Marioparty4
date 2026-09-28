#include "table_scene.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace quest {
namespace {

// The scan returns its entities, each with one label or more, comma separated.
bool has_label(const std::string& labels, const char* wanted) {
  size_t start = 0;
  while (start <= labels.size()) {
    const size_t end = std::min(labels.find(',', start), labels.size());
    if (labels.compare(start, end - start, wanted) == 0) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

} // namespace

const char* const TableScene::kExtension = XR_FB_SCENE_EXTENSION_NAME;
const char* const TableScene::kCaptureExtension = XR_FB_SCENE_CAPTURE_EXTENSION_NAME;

void TableScene::init(XrInstance instance, XrSession session, XrSpace stage, bool capture) {
  mInstance = instance;
  mSession = session;
  mStage = stage;
  mQuery = proc<PFN_xrQuerySpacesFB>(instance, "xrQuerySpacesFB");
  mRetrieve = proc<PFN_xrRetrieveSpaceQueryResultsFB>(instance, "xrRetrieveSpaceQueryResultsFB");
  mLabels = proc<PFN_xrGetSpaceSemanticLabelsFB>(instance, "xrGetSpaceSemanticLabelsFB");
  mBounds = proc<PFN_xrGetSpaceBoundingBox2DFB>(instance, "xrGetSpaceBoundingBox2DFB");
  mGetStatus = proc<PFN_xrGetSpaceComponentStatusFB>(instance, "xrGetSpaceComponentStatusFB");
  mSetStatus = proc<PFN_xrSetSpaceComponentStatusFB>(instance, "xrSetSpaceComponentStatusFB");
  mCapture = capture ? proc<PFN_xrRequestSceneCaptureFB>(instance, "xrRequestSceneCaptureFB") : nullptr;
  if (mQuery == nullptr || mRetrieve == nullptr || mLabels == nullptr || mBounds == nullptr || mGetStatus == nullptr ||
      mSetStatus == nullptr) {
    LOGW("Room scan unavailable: the table is measured with the controller only");
    mQuery = nullptr;
    mCapture = nullptr;
  }
}

void TableScene::destroy() {
  clear();
  for (const Found& table : mPending) {
    xrDestroySpace(table.space);
  }
  mPending.clear();
  mQuery = nullptr;
  mCapture = nullptr;
}

void TableScene::clear() {
  for (const Found& table : mTables) {
    xrDestroySpace(table.space);
  }
  mTables.clear();
}

void TableScene::set_permission(bool granted) {
  if (granted == mPermission) {
    return;
  }
  mPermission = granted;
  LOGI("Spatial data permission %s", granted ? "granted" : "refused");
  if (granted) {
    query();
  }
}

void TableScene::query() {
  if (mQuery == nullptr || !mPermission) {
    return;
  }
  // The scene's entities carry semantic labels; tables are picked among them.
  XrSpaceStorageLocationFilterInfoFB location{XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB};
  location.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
  XrSpaceComponentFilterInfoFB filter{XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB};
  filter.next = &location;
  filter.componentType = XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB;
  XrSpaceQueryInfoFB info{XR_TYPE_SPACE_QUERY_INFO_FB};
  info.queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB;
  info.maxResultCount = 256;
  info.filter = reinterpret_cast<const XrSpaceFilterInfoBaseHeaderFB*>(&filter);
  for (const Found& table : mPending) {
    xrDestroySpace(table.space);
  }
  mPending.clear();
  mQueried = false;
  if (!check(mInstance, mQuery(mSession, reinterpret_cast<const XrSpaceQueryInfoBaseHeaderFB*>(&info), &mQueryRequest),
             "xrQuerySpacesFB (room scan)")) {
    mQueryRequest = 0;
    mQueried = true;
  }
}

bool TableScene::request_capture() {
  if (!can_capture() || mCapturing) {
    return false;
  }
  XrSceneCaptureRequestInfoFB info{XR_TYPE_SCENE_CAPTURE_REQUEST_INFO_FB};
  if (!check(mInstance, mCapture(mSession, &info, &mCaptureRequest), "xrRequestSceneCaptureFB")) {
    return false;
  }
  LOGI("Room scan: Space Setup opened");
  mCapturing = true;
  return true;
}

// Keeps a query result if the scan labels it a table and gives it a size.
void TableScene::add(XrSpace space) {
  XrSpaceComponentStatusFB status{XR_TYPE_SPACE_COMPONENT_STATUS_FB};
  if (XR_FAILED(mGetStatus(space, XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB, &status)) || !status.enabled) {
    xrDestroySpace(space);
    return;
  }
  // Only tables matter; the scan's older "DESK" label comes back as a table.
  XrSemanticLabelsSupportInfoFB support{XR_TYPE_SEMANTIC_LABELS_SUPPORT_INFO_FB};
  support.flags = XR_SEMANTIC_LABELS_SUPPORT_MULTIPLE_SEMANTIC_LABELS_BIT_FB |
                  XR_SEMANTIC_LABELS_SUPPORT_ACCEPT_DESK_TO_TABLE_MIGRATION_BIT_FB;
  support.recognizedLabels = "TABLE,OTHER";
  XrSemanticLabelsFB labels{XR_TYPE_SEMANTIC_LABELS_FB};
  labels.next = &support;
  std::string text;
  if (XR_SUCCEEDED(mLabels(mSession, space, &labels)) && labels.bufferCountOutput > 0) {
    text.resize(labels.bufferCountOutput);
    labels.bufferCapacityInput = labels.bufferCountOutput;
    labels.buffer = text.data();
    if (XR_FAILED(mLabels(mSession, space, &labels))) {
      text.clear();
    }
    text.resize(std::strlen(text.c_str()));
  }
  XrRect2Df bounds{};
  if (!has_label(text, "TABLE") || XR_FAILED(mBounds(mSession, space, &bounds))) {
    xrDestroySpace(space);
    return;
  }
  // Scene entities are locatable already; enable it otherwise (asynchronous,
  // located from the next frames on).
  XrSpaceComponentStatusFB locatable{XR_TYPE_SPACE_COMPONENT_STATUS_FB};
  if (XR_SUCCEEDED(mGetStatus(space, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB, &locatable)) && !locatable.enabled &&
      !locatable.changePending) {
    XrSpaceComponentStatusSetInfoFB enable{XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB};
    enable.componentType = XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB;
    enable.enabled = XR_TRUE;
    XrAsyncRequestIdFB request = 0;
    check(mInstance, mSetStatus(space, &enable, &request), "xrSetSpaceComponentStatusFB (scan table)");
  }
  LOGI("Room scan: table %.2f x %.2f m", bounds.extent.width, bounds.extent.height);
  mPending.push_back({space, bounds}); // the previous tables stay until the query completes
}

bool TableScene::handle_event(const XrEventDataBuffer& event) {
  if (mQuery == nullptr) {
    return false;
  }
  switch (event.type) {
  case XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB: {
    const auto& available = reinterpret_cast<const XrEventDataSpaceQueryResultsAvailableFB&>(event);
    if (available.requestId != mQueryRequest || mQueryRequest == 0) {
      return false;
    }
    XrSpaceQueryResultsFB results{XR_TYPE_SPACE_QUERY_RESULTS_FB};
    if (XR_FAILED(mRetrieve(mSession, available.requestId, &results)) || results.resultCountOutput == 0) {
      return true;
    }
    std::vector<XrSpaceQueryResultFB> found(results.resultCountOutput);
    results.resultCapacityInput = static_cast<uint32_t>(found.size());
    results.results = found.data();
    if (!check(mInstance, mRetrieve(mSession, available.requestId, &results), "xrRetrieveSpaceQueryResultsFB (room scan)")) {
      return true;
    }
    for (uint32_t i = 0; i < results.resultCountOutput; ++i) {
      add(found[i].space);
    }
    return true;
  }
  case XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB: {
    const auto& done = reinterpret_cast<const XrEventDataSpaceQueryCompleteFB&>(event);
    if (done.requestId != mQueryRequest || mQueryRequest == 0) {
      return false;
    }
    clear();
    mTables = std::move(mPending);
    mPending.clear();
    mQueryRequest = 0;
    mQueried = true;
    LOGI("Room scan: %zu table(s) (query result %d)", mTables.size(), static_cast<int>(done.result));
    return true;
  }
  case XR_TYPE_EVENT_DATA_SCENE_CAPTURE_COMPLETE_FB: {
    const auto& done = reinterpret_cast<const XrEventDataSceneCaptureCompleteFB&>(event);
    if (done.requestId != mCaptureRequest) {
      return false;
    }
    mCapturing = false;
    LOGI("Room scan: Space Setup closed (%d)", static_cast<int>(done.result));
    query();
    return true;
  }
  case XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB: {
    const auto& done = reinterpret_cast<const XrEventDataSpaceSetStatusCompleteFB&>(event);
    const auto ours = [&](const Found& table) { return table.space == done.space; };
    return std::ranges::any_of(mPending, ours) || std::ranges::any_of(mTables, ours);
  }
  default:
    return false;
  }
}

std::vector<ScannedTable> TableScene::tables(XrTime time) const {
  std::vector<ScannedTable> out;
  for (const Found& table : mTables) {
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_FAILED(xrLocateSpace(table.space, mStage, time, &location)) || (location.locationFlags & kValid) != kValid) {
      continue;
    }
    const XrPosef& p = location.pose;
    const float position[3] = {p.position.x, p.position.y, p.position.z};
    const float orientation[4] = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    ScannedTable top;
    if (table_from_plane(position, orientation, table.bounds.offset.x, table.bounds.offset.y, table.bounds.extent.width,
                         table.bounds.extent.height, top)) {
      out.push_back(top);
    }
  }
  return out;
}

TableScene::State TableScene::state() const {
  if (mQuery == nullptr) {
    return State::Unavailable;
  }
  if (mCapturing) {
    return State::Capturing;
  }
  if (!mPermission) {
    return State::NoPermission;
  }
  if (!mQueried) {
    return State::Searching;
  }
  return mTables.empty() ? State::NoTable : State::Found;
}

void TableScene::largest(float& length, float& width) const {
  length = width = 0.0f;
  for (const Found& table : mTables) {
    const float a = std::max(table.bounds.extent.width, table.bounds.extent.height);
    const float b = std::min(table.bounds.extent.width, table.bounds.extent.height);
    if (a * b > length * width) {
      length = a;
      width = b;
    }
  }
}

} // namespace quest
