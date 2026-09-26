#include "table_anchor.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace quest {
namespace {

std::string to_hex(const XrUuidEXT& uuid) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (uint8_t byte : uuid.data) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 15]);
  }
  return out;
}

bool from_hex(const std::string& hex, XrUuidEXT& uuid) {
  if (hex.size() != XR_UUID_SIZE_EXT * 2) {
    return false;
  }
  for (size_t i = 0; i < XR_UUID_SIZE_EXT; ++i) {
    unsigned value = 0;
    if (std::sscanf(hex.c_str() + i * 2, "%2x", &value) != 1) {
      return false;
    }
    uuid.data[i] = static_cast<uint8_t>(value);
  }
  return true;
}

} // namespace

const char* const TableAnchor::kExtensions[3] = {
    XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_STORAGE_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME,
};

// A few "key values" lines in the app's files (QuestVr passes the path).
bool TableSettings::load(const std::string& path) {
  std::ifstream in{path};
  if (!in) {
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields{line};
    std::string key;
    fields >> key;
    if (key == "pose") {
      XrPosef& p = pose;
      placed = static_cast<bool>(fields >> p.position.x >> p.position.y >> p.position.z >> p.orientation.x >>
                                 p.orientation.y >> p.orientation.z >> p.orientation.w);
    } else if (key == "width") {
      fields >> screenWidth;
    } else if (key == "resolution") {
      fields >> resolution;
    } else if (key == "passthrough") {
      int on = 1;
      fields >> on;
      passthrough = on != 0;
    } else if (key == "anchor") {
      fields >> anchorUuid;
    } else if (key == "diorama") {
      int on = 1;
      fields >> on;
      diorama = on != 0;
    } else if (key == "model_scale") {
      fields >> modelScale;
    }
  }
  if (resolution != 1080 && resolution != 1440 && resolution != 2160) {
    resolution = 1080;
  }
  if (!(screenWidth >= 0.3f && screenWidth <= 4.0f)) {
    screenWidth = 1.0f;
  }
  if (!(modelScale >= 0.00003f && modelScale <= 0.002f)) {
    modelScale = 0.00025f;
  }
  return true;
}

void TableSettings::save(const std::string& path) const {
  // Written aside then renamed: a crash never leaves half a file.
  const std::string temporary = path + ".tmp";
  {
    std::ofstream out{temporary, std::ios::trunc};
    if (!out) {
      LOGW("Unable to save the table placement");
      return;
    }
    if (placed) {
      const XrPosef& p = pose;
      out << "pose " << p.position.x << ' ' << p.position.y << ' ' << p.position.z << ' ' << p.orientation.x << ' '
          << p.orientation.y << ' ' << p.orientation.z << ' ' << p.orientation.w << '\n';
    }
    out << "width " << screenWidth << '\n';
    out << "resolution " << resolution << '\n';
    out << "passthrough " << (passthrough ? 1 : 0) << '\n';
    out << "diorama " << (diorama ? 1 : 0) << '\n';
    out << "model_scale " << modelScale << '\n';
    if (!anchorUuid.empty()) {
      out << "anchor " << anchorUuid << '\n';
    }
  }
  std::rename(temporary.c_str(), path.c_str());
}

void TableAnchor::init(XrInstance instance, XrSession session, XrSpace stage) {
  mInstance = instance;
  mSession = session;
  mStage = stage;
  mCreate = proc<PFN_xrCreateSpatialAnchorFB>(instance, "xrCreateSpatialAnchorFB");
  mSetStatus = proc<PFN_xrSetSpaceComponentStatusFB>(instance, "xrSetSpaceComponentStatusFB");
  mGetStatus = proc<PFN_xrGetSpaceComponentStatusFB>(instance, "xrGetSpaceComponentStatusFB");
  mSave = proc<PFN_xrSaveSpaceFB>(instance, "xrSaveSpaceFB");
  mErase = proc<PFN_xrEraseSpaceFB>(instance, "xrEraseSpaceFB");
  mQuery = proc<PFN_xrQuerySpacesFB>(instance, "xrQuerySpacesFB");
  mRetrieve = proc<PFN_xrRetrieveSpaceQueryResultsFB>(instance, "xrRetrieveSpaceQueryResultsFB");
  if (mCreate == nullptr || mSetStatus == nullptr || mGetStatus == nullptr || mSave == nullptr ||
      mErase == nullptr || mQuery == nullptr || mRetrieve == nullptr) {
    LOGW("Spatial anchors unavailable: the placement is kept relative to the play area");
    mCreate = nullptr;
  }
}

void TableAnchor::destroy() {
  for (XrSpace* space : {&mSpace, &mErasing}) {
    if (*space != XR_NULL_HANDLE) {
      xrDestroySpace(*space);
      *space = XR_NULL_HANDLE;
    }
  }
  mReady = false;
}

void TableAnchor::load(const std::string& uuidHex) {
  XrUuidEXT uuid{};
  if (mCreate == nullptr || !from_hex(uuidHex, uuid)) {
    return;
  }
  XrSpaceStorageLocationFilterInfoFB location{XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB};
  location.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
  XrSpaceUuidFilterInfoFB filter{XR_TYPE_SPACE_UUID_FILTER_INFO_FB};
  filter.next = &location;
  filter.uuidCount = 1;
  filter.uuids = &uuid;
  XrSpaceQueryInfoFB query{XR_TYPE_SPACE_QUERY_INFO_FB};
  query.queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB;
  query.maxResultCount = 1;
  query.filter = reinterpret_cast<const XrSpaceFilterInfoBaseHeaderFB*>(&filter);
  check(mInstance, mQuery(mSession, reinterpret_cast<const XrSpaceQueryInfoBaseHeaderFB*>(&query), &mQueryRequest),
        "xrQuerySpacesFB");
}

void TableAnchor::place(const XrPosef& pose, XrTime time) {
  if (mCreate == nullptr) {
    return;
  }
  if (mSpace != XR_NULL_HANDLE) {
    erase(mSpace);
    mSpace = XR_NULL_HANDLE;
  }
  mReady = false;
  XrSpatialAnchorCreateInfoFB info{XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_FB};
  info.space = mStage;
  info.poseInSpace = pose;
  info.time = time;
  check(mInstance, mCreate(mSession, &info, &mCreateRequest), "xrCreateSpatialAnchorFB");
}

bool TableAnchor::locate(XrTime time, XrPosef& pose) const {
  if (!mReady || mSpace == XR_NULL_HANDLE) {
    return false;
  }
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(mSpace, mStage, time, &location))) {
    return false;
  }
  constexpr XrSpaceLocationFlags kTracked = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if ((location.locationFlags & kTracked) != kTracked) {
    return false;
  }
  pose = location.pose;
  return true;
}

bool TableAnchor::take_saved_uuid(std::string& uuid) {
  if (!mSavedPending) {
    return false;
  }
  mSavedPending = false;
  uuid = mSavedUuid;
  return true;
}

// Enables a component, or goes on at once when it already is.
void TableAnchor::enable_component(XrSpace space, XrSpaceComponentTypeFB component) {
  XrSpaceComponentStatusFB status{XR_TYPE_SPACE_COMPONENT_STATUS_FB};
  if (XR_SUCCEEDED(mGetStatus(space, component, &status)) && status.enabled && !status.changePending) {
    if (component == XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB) {
      mReady = space == mSpace;
    } else if (component == XR_SPACE_COMPONENT_TYPE_STORABLE_FB) {
      save(space);
    }
    return;
  }
  XrSpaceComponentStatusSetInfoFB info{XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB};
  info.componentType = component;
  info.enabled = XR_TRUE;
  XrAsyncRequestIdFB request = 0;
  check(mInstance, mSetStatus(space, &info, &request), "xrSetSpaceComponentStatusFB");
}

void TableAnchor::save(XrSpace space) {
  XrSpaceSaveInfoFB info{XR_TYPE_SPACE_SAVE_INFO_FB};
  info.space = space;
  info.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
  info.persistenceMode = XR_SPACE_PERSISTENCE_MODE_INDEFINITE_FB;
  XrAsyncRequestIdFB request = 0;
  check(mInstance, mSave(mSession, &info, &request), "xrSaveSpaceFB");
}

void TableAnchor::erase(XrSpace space) {
  if (mErasing != XR_NULL_HANDLE) {
    xrDestroySpace(mErasing);
  }
  mErasing = space;
  XrSpaceEraseInfoFB info{XR_TYPE_SPACE_ERASE_INFO_FB};
  info.space = space;
  info.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
  XrAsyncRequestIdFB request = 0;
  if (!check(mInstance, mErase(mSession, &info, &request), "xrEraseSpaceFB")) {
    xrDestroySpace(mErasing);
    mErasing = XR_NULL_HANDLE;
  }
}

bool TableAnchor::handle_event(const XrEventDataBuffer& event) {
  if (mCreate == nullptr) {
    return false;
  }
  switch (event.type) {
  case XR_TYPE_EVENT_DATA_SPATIAL_ANCHOR_CREATE_COMPLETE_FB: {
    const auto& done = reinterpret_cast<const XrEventDataSpatialAnchorCreateCompleteFB&>(event);
    if (done.requestId != mCreateRequest) {
      return true;
    }
    if (!check(mInstance, done.result, "Anchor creation")) {
      return true;
    }
    mSpace = done.space;
    enable_component(mSpace, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB);
    enable_component(mSpace, XR_SPACE_COMPONENT_TYPE_STORABLE_FB);
    return true;
  }
  case XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB: {
    const auto& done = reinterpret_cast<const XrEventDataSpaceSetStatusCompleteFB&>(event);
    if (done.space != mSpace || !check(mInstance, done.result, "Anchor component")) {
      return true;
    }
    if (done.componentType == XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB) {
      mReady = true;
    } else if (done.componentType == XR_SPACE_COMPONENT_TYPE_STORABLE_FB) {
      save(done.space);
    }
    return true;
  }
  case XR_TYPE_EVENT_DATA_SPACE_SAVE_COMPLETE_FB: {
    const auto& done = reinterpret_cast<const XrEventDataSpaceSaveCompleteFB&>(event);
    if (done.space == mSpace && check(mInstance, done.result, "Anchor save")) {
      mSavedUuid = to_hex(done.uuid);
      mSavedPending = true;
      LOGI("Table anchor saved (%s)", mSavedUuid.c_str());
    }
    return true;
  }
  case XR_TYPE_EVENT_DATA_SPACE_ERASE_COMPLETE_FB: {
    const auto& done = reinterpret_cast<const XrEventDataSpaceEraseCompleteFB&>(event);
    if (done.space == mErasing) {
      xrDestroySpace(mErasing);
      mErasing = XR_NULL_HANDLE;
    }
    return true;
  }
  case XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB: {
    const auto& available = reinterpret_cast<const XrEventDataSpaceQueryResultsAvailableFB&>(event);
    if (available.requestId != mQueryRequest) {
      return true;
    }
    XrSpaceQueryResultsFB results{XR_TYPE_SPACE_QUERY_RESULTS_FB};
    if (XR_FAILED(mRetrieve(mSession, available.requestId, &results)) || results.resultCountOutput == 0) {
      return true;
    }
    std::vector<XrSpaceQueryResultFB> found(results.resultCountOutput);
    results.resultCapacityInput = static_cast<uint32_t>(found.size());
    results.results = found.data();
    if (!check(mInstance, mRetrieve(mSession, available.requestId, &results), "xrRetrieveSpaceQueryResultsFB") ||
        results.resultCountOutput == 0 || mSpace != XR_NULL_HANDLE) {
      return true;
    }
    mSpace = found[0].space;
    enable_component(mSpace, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB);
    LOGI("Table anchor found");
    return true;
  }
  case XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB: {
    const auto& done = reinterpret_cast<const XrEventDataSpaceQueryCompleteFB&>(event);
    if (done.requestId == mQueryRequest && mSpace == XR_NULL_HANDLE) {
      LOGW("Table anchor not found: using the saved position");
    }
    return true;
  }
  default:
    return false;
  }
}

} // namespace quest
