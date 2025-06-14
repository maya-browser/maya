/* vim: se cin sw=2 ts=2 et : */
/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ArrayUtils.h"

#include "GfxInfoBase.h"

#include <mutex>  // std::call_once

#include "GfxDriverInfo.h"
#include "js/Array.h"               // JS::GetArrayLength, JS::NewArrayObject
#include "js/PropertyAndElement.h"  // JS_SetElement, JS_SetProperty
#include "nsCOMPtr.h"
#include "nsCOMArray.h"
#include "nsIPropertyBag2.h"
#include "nsString.h"
#include "nsUnicharUtils.h"
#include "nsVersionComparator.h"
#include "mozilla/Services.h"
#include "mozilla/Observer.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsTArray.h"
#include "nsXULAppAPI.h"
#include "nsIXULAppInfo.h"
#include "mozilla/BinarySearch.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/LookAndFeel.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/BuildConstants.h"
#include "mozilla/gfx/GPUProcessManager.h"
#include "mozilla/gfx/Logging.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/widget/ScreenManager.h"
#include "mozilla/widget/Screen.h"

#include "jsapi.h"

#include "gfxPlatform.h"
#include "gfxConfig.h"
#include "DriverCrashGuard.h"

#ifdef MOZ_WIDGET_ANDROID
#  include <set>
#  include "AndroidBuild.h"
#endif

#if defined(XP_MACOSX)
#  include "nsCocoaFeatures.h"
#endif

using namespace mozilla::widget;
using namespace mozilla;
using mozilla::MutexAutoLock;

StaticAutoPtr<nsTArray<RefPtr<GfxDriverInfo>>> GfxInfoBase::sDriverInfo;
StaticAutoPtr<nsTArray<gfx::GfxInfoFeatureStatus>> GfxInfoBase::sFeatureStatus;
bool GfxInfoBase::sDriverInfoObserverInitialized;
bool GfxInfoBase::sShutdownOccurred;

// Call this when setting sFeatureStatus to a non-null pointer to
// ensure destruction even if the GfxInfo component is never instantiated.
static void InitFeatureStatus(nsTArray<gfx::GfxInfoFeatureStatus>* aPtr) {
  static std::once_flag sOnce;
  std::call_once(sOnce, [] { ClearOnShutdown(&GfxInfoBase::sFeatureStatus); });
  GfxInfoBase::sFeatureStatus = aPtr;
}

// Observes for shutdown so that the child GfxDriverInfo list is freed.
class ShutdownObserver : public nsIObserver {
  virtual ~ShutdownObserver() = default;

 public:
  ShutdownObserver() = default;

  NS_DECL_ISUPPORTS

  NS_IMETHOD Observe(nsISupports* subject, const char* aTopic,
                     const char16_t* aData) override {
    MOZ_ASSERT(strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID) == 0);

    GfxInfoBase::sDriverInfo = nullptr;

    for (auto& deviceFamily : GfxDriverInfo::sDeviceFamilies) {
      deviceFamily = nullptr;
    }

    for (auto& windowProtocol : GfxDriverInfo::sWindowProtocol) {
      delete windowProtocol;
      windowProtocol = nullptr;
    }

    for (auto& deviceVendor : GfxDriverInfo::sDeviceVendors) {
      delete deviceVendor;
      deviceVendor = nullptr;
    }

    for (auto& driverVendor : GfxDriverInfo::sDriverVendors) {
      delete driverVendor;
      driverVendor = nullptr;
    }

    GfxInfoBase::sShutdownOccurred = true;

    return NS_OK;
  }
};

NS_IMPL_ISUPPORTS(ShutdownObserver, nsIObserver)

static void InitGfxDriverInfoShutdownObserver() {
  if (GfxInfoBase::sDriverInfoObserverInitialized) return;

  GfxInfoBase::sDriverInfoObserverInitialized = true;

  nsCOMPtr<nsIObserverService> observerService = services::GetObserverService();
  if (!observerService) {
    NS_WARNING("Could not get observer service!");
    return;
  }

  ShutdownObserver* obs = new ShutdownObserver();
  observerService->AddObserver(obs, NS_XPCOM_SHUTDOWN_OBSERVER_ID, false);
}

using namespace mozilla::widget;
using namespace mozilla::gfx;
using namespace mozilla;

NS_IMPL_ISUPPORTS(GfxInfoBase, nsIGfxInfo, nsIObserver,
                  nsISupportsWeakReference)

#define BLOCKLIST_PREF_BRANCH "gfx.blacklist."
#define SUGGESTED_VERSION_PREF BLOCKLIST_PREF_BRANCH "suggested-driver-version"

static const char* GetPrefNameForFeature(int32_t aFeature) {
  const char* fullpref = nullptr;
  switch (aFeature) {
#define GFXINFO_FEATURE(id, name, pref)    \
  case nsIGfxInfo::FEATURE_##id:           \
    fullpref = BLOCKLIST_PREF_BRANCH pref; \
    break;
#include "mozilla/widget/GfxInfoFeatureDefs.h"
#undef GFXINFO_FEATURE
    default:
      MOZ_ASSERT_UNREACHABLE("Unexpected nsIGfxInfo feature?!");
      break;
  }

  return fullpref;
}

// Returns the value of the pref for the relevant feature in aValue.
// If the pref doesn't exist, aValue is not touched, and returns false.
static bool GetPrefValueForFeature(int32_t aFeature, int32_t& aValue,
                                   nsACString& aFailureId) {
  const char* prefname = GetPrefNameForFeature(aFeature);
  if (!prefname) return false;

  aValue = nsIGfxInfo::FEATURE_STATUS_UNKNOWN;
  if (!NS_SUCCEEDED(Preferences::GetInt(prefname, &aValue))) {
    return false;
  }

  if (aValue == nsIGfxInfo::FEATURE_DENIED) {
    // We should never see the DENIED status with the downloadable blocklist.
    return false;
  }

  nsCString failureprefname(prefname);
  failureprefname += ".failureid";
  nsAutoCString failureValue;
  nsresult rv = Preferences::GetCString(failureprefname.get(), failureValue);
  if (NS_SUCCEEDED(rv)) {
    aFailureId = failureValue.get();
  } else {
    aFailureId = "FEATURE_FAILURE_BLOCKLIST_PREF";
  }

  return true;
}

static void SetPrefValueForFeature(int32_t aFeature, int32_t aValue,
                                   const nsACString& aFailureId) {
  const char* prefname = GetPrefNameForFeature(aFeature);
  if (!prefname) return;
  if (XRE_IsParentProcess()) {
    GfxInfoBase::sFeatureStatus = nullptr;
  }

  Preferences::SetInt(prefname, aValue);
  if (!aFailureId.IsEmpty()) {
    nsAutoCString failureprefname(prefname);
    failureprefname += ".failureid";
    Preferences::SetCString(failureprefname.get(), aFailureId);
  }
}

static void RemovePrefForFeature(int32_t aFeature) {
  const char* prefname = GetPrefNameForFeature(aFeature);
  if (!prefname) return;

  if (XRE_IsParentProcess()) {
    GfxInfoBase::sFeatureStatus = nullptr;
  }
  Preferences::ClearUser(prefname);
}

static bool GetPrefValueForDriverVersion(nsCString& aVersion) {
  return NS_SUCCEEDED(
      Preferences::GetCString(SUGGESTED_VERSION_PREF, aVersion));
}

static void SetPrefValueForDriverVersion(const nsAString& aVersion) {
  Preferences::SetString(SUGGESTED_VERSION_PREF, aVersion);
}

static void RemovePrefForDriverVersion() {
  Preferences::ClearUser(SUGGESTED_VERSION_PREF);
}

static OperatingSystem BlocklistOSToOperatingSystem(const nsAString& os) {
#define GFXINFO_OS(id, name)     \
  if (os.Equals(u##name##_ns)) { \
    return OperatingSystem::id;  \
  }
#include "mozilla/widget/GfxInfoOperatingSystemDefs.h"
#undef GFXINFO_OS
  return OperatingSystem::Unknown;
}

static RefreshRateStatus BlocklistToRefreshRateStatus(
    const nsAString& refreshRateStatus) {
#define GFXINFO_REFRESH_RATE_STATUS(id, name)   \
  if (refreshRateStatus.Equals(u##name##_ns)) { \
    return RefreshRateStatus::id;               \
  }
#include "mozilla/widget/GfxInfoRefreshRateStatusDefs.h"
#undef GFXINFO_OS
  return RefreshRateStatus::Unknown;
}

static already_AddRefed<const GfxDeviceFamily> BlocklistDevicesToDeviceFamily(
    nsTArray<nsCString>& devices) {
  if (devices.Length() == 0) return nullptr;

  // For each device, get its device ID, and return a freshly-allocated
  // GfxDeviceFamily with the contents of that array.
  auto deviceIds = MakeRefPtr<GfxDeviceFamily>();

  for (uint32_t i = 0; i < devices.Length(); ++i) {
    // We make sure we don't add any "empty" device entries to the array, so
    // we don't need to check if devices[i] is empty.
    deviceIds->Append(NS_ConvertUTF8toUTF16(devices[i]));
  }

  return deviceIds.forget();
}

static int32_t BlocklistFeatureToGfxFeature(const nsAString& aFeature) {
  MOZ_ASSERT(!aFeature.IsEmpty());
#define GFXINFO_FEATURE(id, name, pref) \
  if (aFeature.Equals(u##name##_ns)) {  \
    return nsIGfxInfo::FEATURE_##id;    \
  }
#include "mozilla/widget/GfxInfoFeatureDefs.h"
#undef GFXINFO_FEATURE

  // If we don't recognize the feature, it may be new, and something
  // this version doesn't understand.  So, nothing to do.  This is
  // different from feature not being specified at all, in which case
  // this method should not get called and we should continue with the
  // "optional features" blocklisting.
  return nsIGfxInfo::FEATURE_INVALID;
}

static int32_t BlocklistFeatureStatusToGfxFeatureStatus(
    const nsAString& aStatus) {
#define GFXINFO_FEATURE_STATUS(id)    \
  if (aStatus.Equals(u## #id##_ns)) { \
    return nsIGfxInfo::FEATURE_##id;  \
  }
#include "mozilla/widget/GfxInfoFeatureStatusDefs.h"
#undef GFXINFO_FEATURE_STATUS
  return nsIGfxInfo::FEATURE_STATUS_OK;
}

static void GfxFeatureStatusToBlocklistFeatureStatus(int32_t aStatus,
                                                     nsAString& aStatusOut) {
  switch (aStatus) {
#define GFXINFO_FEATURE_STATUS(id)   \
  case nsIGfxInfo::FEATURE_##id:     \
    aStatusOut.Assign(u## #id##_ns); \
    break;
#include "mozilla/widget/GfxInfoFeatureStatusDefs.h"
#undef GFXINFO_FEATURE
    default:
      MOZ_ASSERT_UNREACHABLE("Unexpected feature status!");
      break;
  }
}

static VersionComparisonOp BlocklistComparatorToComparisonOp(
    const nsAString& op) {
#define GFXINFO_DRIVER_VERSION_CMP(id) \
  if (op.Equals(u## #id##_ns)) {       \
    return DRIVER_##id;                \
  }
#include "mozilla/widget/GfxInfoDriverVersionCmpDefs.h"
#undef GFXINFO_DRIVER_VERSION_CMP

  // The default is to ignore it.
  return DRIVER_COMPARISON_IGNORED;
}

/*
  Deserialize Blocklist entries from string.
  e.g:
  os:WINNT 6.0\tvendor:0x8086\tdevices:0x2582,0x2782\tfeature:DIRECT3D_10_LAYERS\tfeatureStatus:BLOCKED_DRIVER_VERSION\tdriverVersion:8.52.322.2202\tdriverVersionComparator:LESS_THAN_OR_EQUAL
*/
static bool BlocklistEntryToDriverInfo(const nsACString& aBlocklistEntry,
                                       GfxDriverInfo* aDriverInfo) {
  // If we get an application version to be zero, something is not working
  // and we are not going to bother checking the blocklist versions.
  // See TestGfxWidgets.cpp for how version comparison works.
  // <versionRange minVersion="42.0a1" maxVersion="45.0"></versionRange>
  static mozilla::Version zeroV("0");
  static mozilla::Version appV(GfxInfoBase::GetApplicationVersion().get());
  if (appV <= zeroV) {
    gfxCriticalErrorOnce(gfxCriticalError::DefaultOptions(false))
        << "Invalid application version "
        << GfxInfoBase::GetApplicationVersion().get();
  }

  aDriverInfo->mRuleId = "FEATURE_FAILURE_DL_BLOCKLIST_NO_ID"_ns;

  for (const auto& keyValue : aBlocklistEntry.Split('\t')) {
    nsTArray<nsCString> splitted;
    ParseString(keyValue, ':', splitted);
    if (splitted.Length() != 2) {
      // If we don't recognize the input data, we do not want to proceed.
      gfxCriticalErrorOnce(CriticalLog::DefaultOptions(false))
          << "Unrecognized data " << nsCString(keyValue).get();
      return false;
    }
    const nsCString& key = splitted[0];
    const nsCString& value = splitted[1];
    NS_ConvertUTF8toUTF16 dataValue(value);

    if (value.Length() == 0) {
      // Safety check for empty values.
      gfxCriticalErrorOnce(CriticalLog::DefaultOptions(false))
          << "Empty value for " << key.get();
      return false;
    }

    if (key.EqualsLiteral("blockID")) {
      nsCString blockIdStr = "FEATURE_FAILURE_DL_BLOCKLIST_"_ns + value;
      aDriverInfo->mRuleId = blockIdStr.get();
    } else if (key.EqualsLiteral("os")) {
      aDriverInfo->mOperatingSystem = BlocklistOSToOperatingSystem(dataValue);
    } else if (key.EqualsLiteral("osversion")) {
      aDriverInfo->mOperatingSystemVersion = strtoul(value.get(), nullptr, 10);
    } else if (key.EqualsLiteral("osVersionEx")) {
      aDriverInfo->mOperatingSystemVersionEx.Parse(value);
    } else if (key.EqualsLiteral("osVersionExMax")) {
      aDriverInfo->mOperatingSystemVersionExMax.Parse(value);
    } else if (key.EqualsLiteral("osVersionExComparator")) {
      aDriverInfo->mOperatingSystemVersionExComparisonOp =
          BlocklistComparatorToComparisonOp(dataValue);
    } else if (key.EqualsLiteral("refreshRateStatus")) {
      aDriverInfo->mRefreshRateStatus = BlocklistToRefreshRateStatus(dataValue);
    } else if (key.EqualsLiteral("minRefreshRate")) {
      aDriverInfo->mMinRefreshRate = strtoul(value.get(), nullptr, 10);
    } else if (key.EqualsLiteral("minRefreshRateMax")) {
      aDriverInfo->mMinRefreshRateMax = strtoul(value.get(), nullptr, 10);
    } else if (key.EqualsLiteral("minRefreshRateComparator")) {
      aDriverInfo->mMinRefreshRateComparisonOp =
          BlocklistComparatorToComparisonOp(dataValue);
    } else if (key.EqualsLiteral("maxRefreshRate")) {
      aDriverInfo->mMaxRefreshRate = strtoul(value.get(), nullptr, 10);
    } else if (key.EqualsLiteral("maxRefreshRateMax")) {
      aDriverInfo->mMaxRefreshRateMax = strtoul(value.get(), nullptr, 10);
    } else if (key.EqualsLiteral("maxRefreshRateComparator")) {
      aDriverInfo->mMaxRefreshRateComparisonOp =
          BlocklistComparatorToComparisonOp(dataValue);
    } else if (key.EqualsLiteral("windowProtocol")) {
      aDriverInfo->mWindowProtocol = dataValue;
    } else if (key.EqualsLiteral("vendor")) {
      aDriverInfo->mAdapterVendor = dataValue;
    } else if (key.EqualsLiteral("driverVendor")) {
      aDriverInfo->mDriverVendor = dataValue;
    } else if (key.EqualsLiteral("feature")) {
      aDriverInfo->mFeature = BlocklistFeatureToGfxFeature(dataValue);
      if (aDriverInfo->mFeature == nsIGfxInfo::FEATURE_INVALID) {
        // If we don't recognize the feature, we do not want to proceed.
        gfxWarning() << "Unrecognized feature " << value.get();
        return false;
      }
    } else if (key.EqualsLiteral("featureStatus")) {
      aDriverInfo->mFeatureStatus =
          BlocklistFeatureStatusToGfxFeatureStatus(dataValue);
    } else if (key.EqualsLiteral("driverVersion")) {
      uint64_t version;
      if (ParseDriverVersion(dataValue, &version))
        aDriverInfo->mDriverVersion = version;
    } else if (key.EqualsLiteral("driverVersionMax")) {
      uint64_t version;
      if (ParseDriverVersion(dataValue, &version))
        aDriverInfo->mDriverVersionMax = version;
    } else if (key.EqualsLiteral("driverVersionComparator")) {
      aDriverInfo->mComparisonOp = BlocklistComparatorToComparisonOp(dataValue);
    } else if (key.EqualsLiteral("model")) {
      aDriverInfo->mModel = dataValue;
    } else if (key.EqualsLiteral("product")) {
      aDriverInfo->mProduct = dataValue;
    } else if (key.EqualsLiteral("manufacturer")) {
      aDriverInfo->mManufacturer = dataValue;
    } else if (key.EqualsLiteral("hardware")) {
      aDriverInfo->mHardware = dataValue;
    } else if (key.EqualsLiteral("versionRange")) {
      nsTArray<nsCString> versionRange;
      ParseString(value, ',', versionRange);
      if (versionRange.Length() != 2) {
        gfxCriticalErrorOnce(CriticalLog::DefaultOptions(false))
            << "Unrecognized versionRange " << value.get();
        return false;
      }
      const nsCString& minValue = versionRange[0];
      const nsCString& maxValue = versionRange[1];

      mozilla::Version minV(minValue.get());
      mozilla::Version maxV(maxValue.get());

      if (minV > zeroV && !(appV >= minV)) {
        // The version of the application is less than the minimal version
        // this blocklist entry applies to, so we can just ignore it by
        // returning false and letting the caller deal with it.
        return false;
      }
      if (maxV > zeroV && !(appV <= maxV)) {
        // The version of the application is more than the maximal version
        // this blocklist entry applies to, so we can just ignore it by
        // returning false and letting the caller deal with it.
        return false;
      }
    } else if (key.EqualsLiteral("devices")) {
      nsTArray<nsCString> devices;
      ParseString(value, ',', devices);
      aDriverInfo->mDevices = BlocklistDevicesToDeviceFamily(devices);
    }
    // We explicitly ignore unknown elements.
  }

  return true;
}

NS_IMETHODIMP
GfxInfoBase::Observe(nsISupports* aSubject, const char* aTopic,
                     const char16_t* aData) {
  if (strcmp(aTopic, "blocklist-data-gfxItems") == 0) {
    nsTArray<RefPtr<GfxDriverInfo>> driverInfo;
    NS_ConvertUTF16toUTF8 utf8Data(aData);

    for (const auto& blocklistEntry : utf8Data.Split('\n')) {
      auto di = MakeRefPtr<GfxDriverInfo>();
      if (BlocklistEntryToDriverInfo(blocklistEntry, di)) {
        driverInfo.AppendElement(std::move(di));
      }
    }

    EvaluateDownloadedBlocklist(driverInfo);
  }

  return NS_OK;
}

GfxInfoBase::GfxInfoBase() : mScreenPixels(INT64_MAX), mMutex("GfxInfoBase") {}

GfxInfoBase::~GfxInfoBase() = default;

nsresult GfxInfoBase::Init() {
  InitGfxDriverInfoShutdownObserver();

  nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService();
  if (os) {
    os->AddObserver(this, "blocklist-data-gfxItems", true);
  }

  return NS_OK;
}

void GfxInfoBase::GetData() {
  if (mScreenPixels != INT64_MAX) {
    // Already initialized.
    return;
  }

  auto& screenManager = ScreenManager::GetSingleton();
  screenManager.GetTotalScreenPixels(&mScreenPixels);

  if (mScreenCount == 0) {
    const auto& screenList = screenManager.CurrentScreenList();
    mScreenCount = screenList.Length();

    mMinRefreshRate = std::numeric_limits<int32_t>::max();
    mMaxRefreshRate = std::numeric_limits<int32_t>::min();
    for (auto& screen : screenList) {
      int32_t refreshRate = screen->GetRefreshRate();
      mMinRefreshRate = std::min(mMinRefreshRate, refreshRate);
      mMaxRefreshRate = std::max(mMaxRefreshRate, refreshRate);
    }
  }
}

#ifdef DEBUG
NS_IMETHODIMP
GfxInfoBase::SpoofMonitorInfo(uint32_t aScreenCount, int32_t aMinRefreshRate,
                              int32_t aMaxRefreshRate) {
  MOZ_ASSERT(aScreenCount > 0);
  mScreenCount = aScreenCount;
  mMinRefreshRate = aMinRefreshRate;
  mMaxRefreshRate = aMaxRefreshRate;
  return NS_OK;
}
#endif

NS_IMETHODIMP
GfxInfoBase::GetFeatureStatus(int32_t aFeature, nsACString& aFailureId,
                              int32_t* aStatus) {
  // Ignore the gfx.blocklist.all pref on release and beta.
#if defined(RELEASE_OR_BETA)
  int32_t blocklistAll = 0;
#else
  int32_t blocklistAll = StaticPrefs::gfx_blocklist_all_AtStartup();
#endif
  if (blocklistAll > 0) {
    gfxCriticalErrorOnce(gfxCriticalError::DefaultOptions(false))
        << "Forcing blocklisting all features";
    *aStatus = FEATURE_BLOCKED_DEVICE;
    aFailureId = "FEATURE_FAILURE_BLOCK_ALL";
    return NS_OK;
  }

  if (blocklistAll < 0) {
    gfxCriticalErrorOnce(gfxCriticalError::DefaultOptions(false))
        << "Ignoring any feature blocklisting.";
    *aStatus = FEATURE_STATUS_OK;
    return NS_OK;
  }

  // This is how we evaluate the downloadable blocklist. If there is no pref,
  // then we will fallback to checking the static blocklist.
  if (GetPrefValueForFeature(aFeature, *aStatus, aFailureId)) {
    return NS_OK;
  }

  if (XRE_IsContentProcess() || XRE_IsGPUProcess()) {
    // Use the cached data received from the parent process.
    MOZ_ASSERT(sFeatureStatus);
    bool success = false;
    for (const auto& fs : *sFeatureStatus) {
      if (fs.feature() == aFeature) {
        aFailureId = fs.failureId();
        *aStatus = fs.status();
        success = true;
        break;
      }
    }
    return success ? NS_OK : NS_ERROR_FAILURE;
  }

  nsString version;
  nsTArray<RefPtr<GfxDriverInfo>> driverInfo;
  nsresult rv =
      GetFeatureStatusImpl(aFeature, aStatus, version, driverInfo, aFailureId);
  return rv;
}

NS_IMETHODIMP
GfxInfoBase::GetFeatureStatusStr(const nsAString& aFeature,
                                 nsACString& aFailureId, nsAString& aStatus) {
  int32_t feature = BlocklistFeatureToGfxFeature(aFeature);
  if (feature == nsIGfxInfo::FEATURE_INVALID) {
    NS_ConvertUTF16toUTF8 feature(aFeature);
    gfxWarning() << "Unrecognized feature " << feature.get();
    return NS_ERROR_INVALID_ARG;
  }
  int32_t status = nsIGfxInfo::FEATURE_STATUS_UNKNOWN;
  nsresult rv = GetFeatureStatus(feature, aFailureId, &status);
  GfxFeatureStatusToBlocklistFeatureStatus(status, aStatus);
  return rv;
}

nsTArray<gfx::GfxInfoFeatureStatus> GfxInfoBase::GetAllFeatures() {
  MOZ_RELEASE_ASSERT(XRE_IsParentProcess());
  if (!sFeatureStatus) {
    InitFeatureStatus(new nsTArray<gfx::GfxInfoFeatureStatus>());
    for (int32_t i = nsIGfxInfo::FEATURE_START; i < nsIGfxInfo::FEATURE_COUNT;
         ++i) {
      int32_t status = nsIGfxInfo::FEATURE_STATUS_INVALID;
      nsAutoCString failureId;
      GetFeatureStatus(i, failureId, &status);
      gfx::GfxInfoFeatureStatus gfxFeatureStatus;
      gfxFeatureStatus.feature() = i;
      gfxFeatureStatus.status() = status;
      gfxFeatureStatus.failureId() = failureId;
      sFeatureStatus->AppendElement(gfxFeatureStatus);
    }
  }

  nsTArray<gfx::GfxInfoFeatureStatus> features;
  for (const auto& status : *sFeatureStatus) {
    gfx::GfxInfoFeatureStatus copy = status;
    features.AppendElement(copy);
  }
  return features;
}

inline bool MatchingAllowStatus(int32_t aStatus) {
  switch (aStatus) {
    case nsIGfxInfo::FEATURE_ALLOW_ALWAYS:
    case nsIGfxInfo::FEATURE_ALLOW_QUALIFIED:
      return true;
    default:
      return false;
  }
}

// Matching OS go somewhat beyond the simple equality check because of the
// "All Windows" and "All OS X" variations.
//
// aBlockedOS is describing the system(s) we are trying to block.
// aSystemOS is describing the system we are running on.
//
// aSystemOS should not be "Windows" or "OSX" - it should be set to
// a particular version instead.
// However, it is valid for aBlockedOS to be one of those generic values,
// as we could be blocking all of the versions.
inline bool MatchingOperatingSystems(OperatingSystem aBlockedOS,
                                     OperatingSystem aSystemOS) {
  MOZ_ASSERT(aSystemOS != OperatingSystem::Windows &&
             aSystemOS != OperatingSystem::OSX);

  // If the block entry OS is unknown, it doesn't match
  if (aBlockedOS == OperatingSystem::Unknown) {
    return false;
  }

#if defined(XP_WIN)
  if (aBlockedOS == OperatingSystem::Windows) {
    // We do want even "unknown" aSystemOS to fall under "all windows"
    return true;
  }

  if (aBlockedOS == OperatingSystem::Windows10or11 &&
      (aSystemOS == OperatingSystem::Windows10 ||
       aSystemOS == OperatingSystem::Windows11)) {
    return true;
  }
#endif

#if defined(XP_MACOSX)
  if (aBlockedOS == OperatingSystem::OSX) {
    // We do want even "unknown" aSystemOS to fall under "all OS X"
    return true;
  }
#endif

  return aSystemOS == aBlockedOS;
}

/* static */
bool GfxInfoBase::MatchingRefreshRateStatus(RefreshRateStatus aSystemStatus,
                                            RefreshRateStatus aBlockedStatus) {
  switch (aBlockedStatus) {
    case RefreshRateStatus::Any:
      return true;
    case RefreshRateStatus::AnySame:
      return aSystemStatus == RefreshRateStatus::Single ||
             aSystemStatus == RefreshRateStatus::MultipleSame;
    default:
      break;
  }
  return aSystemStatus == aBlockedStatus;
}

/* static */ bool GfxInfoBase::MatchingRefreshRates(int32_t aSystem,
                                                    int32_t aBlocked,
                                                    int32_t aBlockedMax,
                                                    VersionComparisonOp aCmp) {
  switch (aCmp) {
    case DRIVER_COMPARISON_IGNORED:
      return true;
    case DRIVER_LESS_THAN:
      return aSystem < aBlocked;
    case DRIVER_LESS_THAN_OR_EQUAL:
      return aSystem <= aBlocked;
    case DRIVER_GREATER_THAN:
      return aSystem > aBlocked;
    case DRIVER_GREATER_THAN_OR_EQUAL:
      return aSystem >= aBlocked;
    case DRIVER_EQUAL:
      return aSystem == aBlocked;
    case DRIVER_NOT_EQUAL:
      return aSystem != aBlocked;
    case DRIVER_BETWEEN_EXCLUSIVE:
      return aSystem > aBlocked && aSystem < aBlockedMax;
    case DRIVER_BETWEEN_INCLUSIVE:
      return aSystem >= aBlocked && aSystem <= aBlockedMax;
    case DRIVER_BETWEEN_INCLUSIVE_START:
      return aSystem >= aBlocked && aSystem < aBlockedMax;
    default:
      NS_WARNING("Unhandled op in GfxDriverInfo");
      break;
  }
  return false;
}

inline bool MatchingBattery(BatteryStatus aBatteryStatus, bool aHasBattery) {
  switch (aBatteryStatus) {
    case BatteryStatus::All:
      return true;
    case BatteryStatus::None:
      return !aHasBattery;
    case BatteryStatus::Present:
      return aHasBattery;
  }

  MOZ_ASSERT_UNREACHABLE("bad battery status");
  return false;
}

inline bool MatchingScreenSize(ScreenSizeStatus aScreenStatus,
                               int64_t aScreenPixels) {
  constexpr int64_t kMaxSmallPixels = 2304000;   // 1920x1200
  constexpr int64_t kMaxMediumPixels = 4953600;  // 3440x1440

  switch (aScreenStatus) {
    case ScreenSizeStatus::All:
      return true;
    case ScreenSizeStatus::Small:
      return aScreenPixels <= kMaxSmallPixels;
    case ScreenSizeStatus::SmallAndMedium:
      return aScreenPixels <= kMaxMediumPixels;
    case ScreenSizeStatus::Medium:
      return aScreenPixels > kMaxSmallPixels &&
             aScreenPixels <= kMaxMediumPixels;
    case ScreenSizeStatus::MediumAndLarge:
      return aScreenPixels > kMaxSmallPixels;
    case ScreenSizeStatus::Large:
      return aScreenPixels > kMaxMediumPixels;
  }

  MOZ_ASSERT_UNREACHABLE("bad screen status");
  return false;
}

int32_t GfxInfoBase::FindBlocklistedDeviceInList(
    const nsTArray<RefPtr<GfxDriverInfo>>& info, nsAString& aSuggestedVersion,
    int32_t aFeature, nsACString& aFailureId, OperatingSystem os,
    bool aForAllowing) {
  int32_t status = nsIGfxInfo::FEATURE_STATUS_UNKNOWN;

  // Some properties are not available on all platforms.
  nsAutoString windowProtocol;
  nsresult rv = GetWindowProtocol(windowProtocol);
  if (NS_FAILED(rv) && rv != NS_ERROR_NOT_IMPLEMENTED) {
    return 0;
  }

  RefreshRateStatus refreshRateStatus;
  if (mScreenCount <= 1) {
    refreshRateStatus = RefreshRateStatus::Single;
  } else if (mMinRefreshRate == mMaxRefreshRate) {
    refreshRateStatus = RefreshRateStatus::MultipleSame;
  } else {
    refreshRateStatus = RefreshRateStatus::Mixed;
  }

  bool hasBattery = false;
  rv = GetHasBattery(&hasBattery);
  if (NS_FAILED(rv) && rv != NS_ERROR_NOT_IMPLEMENTED) {
    return 0;
  }

  uint32_t osVersion = OperatingSystemVersion();
  GfxVersionEx osVersionEx = OperatingSystemVersionEx();

  // Get the adapters once then reuse below
  nsAutoString adapterVendorID[2];
  nsAutoString adapterDeviceID[2];
  nsAutoString adapterDriverVendor[2];
  nsAutoString adapterDriverVersionString[2];
  bool adapterInfoFailed[2];

  adapterInfoFailed[0] =
      (NS_FAILED(GetAdapterVendorID(adapterVendorID[0])) ||
       NS_FAILED(GetAdapterDeviceID(adapterDeviceID[0])) ||
       NS_FAILED(GetAdapterDriverVendor(adapterDriverVendor[0])) ||
       NS_FAILED(GetAdapterDriverVersion(adapterDriverVersionString[0])));
  adapterInfoFailed[1] =
      (NS_FAILED(GetAdapterVendorID2(adapterVendorID[1])) ||
       NS_FAILED(GetAdapterDeviceID2(adapterDeviceID[1])) ||
       NS_FAILED(GetAdapterDriverVendor2(adapterDriverVendor[1])) ||
       NS_FAILED(GetAdapterDriverVersion2(adapterDriverVersionString[1])));
  // No point in going on if we don't have adapter info
  if (adapterInfoFailed[0] && adapterInfoFailed[1]) {
    return 0;
  }

#if defined(XP_WIN) || defined(ANDROID) || defined(MOZ_WIDGET_GTK)
  uint64_t driverVersion[2] = {0, 0};
  if (!adapterInfoFailed[0]) {
    ParseDriverVersion(adapterDriverVersionString[0], &driverVersion[0]);
  }
  if (!adapterInfoFailed[1]) {
    ParseDriverVersion(adapterDriverVersionString[1], &driverVersion[1]);
  }
#endif

  uint32_t i = 0;
  for (; i < info.Length(); i++) {
    // If the status is FEATURE_ALLOW_*, then it is for the allowlist, not
    // blocklisting. Only consider entries for our search mode.
    if (MatchingAllowStatus(info[i]->mFeatureStatus) != aForAllowing) {
      continue;
    }

    // If we don't have the info for this GPU, no need to check further.
    // It is unclear that we would ever have a mixture of 1st and 2nd
    // GPU, but leaving the code in for that possibility for now.
    // (Actually, currently mGpu2 will never be true, so this can
    // be optimized out.)
    uint32_t infoIndex = info[i]->mGpu2 ? 1 : 0;
    if (adapterInfoFailed[infoIndex]) {
      continue;
    }

    // Do the operating system check first, no point in getting the driver
    // info if we won't need to use it.
    if (!MatchingOperatingSystems(info[i]->mOperatingSystem, os)) {
      continue;
    }

    if (info[i]->mOperatingSystemVersion &&
        info[i]->mOperatingSystemVersion != osVersion) {
      continue;
    }

    if (!osVersionEx.Compare(info[i]->mOperatingSystemVersionEx,
                             info[i]->mOperatingSystemVersionExMax,
                             info[i]->mOperatingSystemVersionExComparisonOp)) {
      continue;
    }

    if (!MatchingRefreshRateStatus(refreshRateStatus,
                                   info[i]->mRefreshRateStatus)) {
      continue;
    }

    if (mScreenCount > 0 &&
        !MatchingRefreshRates(mMinRefreshRate, info[i]->mMinRefreshRate,
                              info[i]->mMinRefreshRateMax,
                              info[i]->mMinRefreshRateComparisonOp)) {
      continue;
    }

    if (mScreenCount > 0 &&
        !MatchingRefreshRates(mMaxRefreshRate, info[i]->mMaxRefreshRate,
                              info[i]->mMaxRefreshRateMax,
                              info[i]->mMaxRefreshRateComparisonOp)) {
      continue;
    }

    if (!MatchingBattery(info[i]->mBattery, hasBattery)) {
      continue;
    }

    if (!MatchingScreenSize(info[i]->mScreen, mScreenPixels)) {
      continue;
    }

    if (!DoesWindowProtocolMatch(info[i]->mWindowProtocol, windowProtocol)) {
      continue;
    }

    if (!DoesVendorMatch(info[i]->mAdapterVendor, adapterVendorID[infoIndex])) {
      continue;
    }

    if (!DoesDriverVendorMatch(info[i]->mDriverVendor,
                               adapterDriverVendor[infoIndex])) {
      continue;
    }

    if (info[i]->mDevices && !info[i]->mDevices->IsEmpty()) {
      nsresult rv = info[i]->mDevices->Contains(adapterDeviceID[infoIndex]);
      if (rv == NS_ERROR_NOT_AVAILABLE) {
        // Not found
        continue;
      }
      if (rv != NS_OK) {
        // Failed to search, allowlist should not match, blocklist should match
        // for safety reasons
        if (aForAllowing) {
          continue;
        }
        break;
      }
    }

    bool match = false;

    if (!info[i]->mHardware.IsEmpty() &&
        !info[i]->mHardware.Equals(Hardware())) {
      continue;
    }
    if (!info[i]->mModel.IsEmpty() && !info[i]->mModel.Equals(Model())) {
      continue;
    }
    if (!info[i]->mProduct.IsEmpty() && !info[i]->mProduct.Equals(Product())) {
      continue;
    }
    if (!info[i]->mManufacturer.IsEmpty() &&
        !info[i]->mManufacturer.Equals(Manufacturer())) {
      continue;
    }

#if defined(XP_WIN) || defined(ANDROID) || defined(MOZ_WIDGET_GTK)
    switch (info[i]->mComparisonOp) {
      case DRIVER_LESS_THAN:
        match = driverVersion[infoIndex] < info[i]->mDriverVersion;
        break;
      case DRIVER_BUILD_ID_LESS_THAN:
        match = (driverVersion[infoIndex] & 0xFFFF) < info[i]->mDriverVersion;
        break;
      case DRIVER_LESS_THAN_OR_EQUAL:
        match = driverVersion[infoIndex] <= info[i]->mDriverVersion;
        break;
      case DRIVER_BUILD_ID_LESS_THAN_OR_EQUAL:
        match = (driverVersion[infoIndex] & 0xFFFF) <= info[i]->mDriverVersion;
        break;
      case DRIVER_GREATER_THAN:
        match = driverVersion[infoIndex] > info[i]->mDriverVersion;
        break;
      case DRIVER_GREATER_THAN_OR_EQUAL:
        match = driverVersion[infoIndex] >= info[i]->mDriverVersion;
        break;
      case DRIVER_EQUAL:
        match = driverVersion[infoIndex] == info[i]->mDriverVersion;
        break;
      case DRIVER_NOT_EQUAL:
        match = driverVersion[infoIndex] != info[i]->mDriverVersion;
        break;
      case DRIVER_BETWEEN_EXCLUSIVE:
        match = driverVersion[infoIndex] > info[i]->mDriverVersion &&
                driverVersion[infoIndex] < info[i]->mDriverVersionMax;
        break;
      case DRIVER_BETWEEN_INCLUSIVE:
        match = driverVersion[infoIndex] >= info[i]->mDriverVersion &&
                driverVersion[infoIndex] <= info[i]->mDriverVersionMax;
        break;
      case DRIVER_BETWEEN_INCLUSIVE_START:
        match = driverVersion[infoIndex] >= info[i]->mDriverVersion &&
                driverVersion[infoIndex] < info[i]->mDriverVersionMax;
        break;
      case DRIVER_COMPARISON_IGNORED:
        // We don't have a comparison op, so we match everything.
        match = true;
        break;
      default:
        NS_WARNING("Bogus op in GfxDriverInfo");
        break;
    }
#else
    // We don't care what driver version it was. We only check OS version and if
    // the device matches.
    match = true;
#endif

    if (match || info[i]->mDriverVersion == GfxDriverInfo::allDriverVersions) {
      if (info[i]->mFeature == GfxDriverInfo::allFeatures ||
          info[i]->mFeature == aFeature ||
          (info[i]->mFeature == GfxDriverInfo::optionalFeatures &&
           OnlyAllowFeatureOnKnownConfig(aFeature))) {
        status = info[i]->mFeatureStatus;
        if (!info[i]->mRuleId.IsEmpty()) {
          aFailureId = info[i]->mRuleId.get();
        } else {
          aFailureId = "FEATURE_FAILURE_DL_BLOCKLIST_NO_ID";
        }
        break;
      }
    }
  }

#if defined(XP_WIN)
  // As a very special case, we block D2D on machines with an NVidia 310M GPU
  // as either the primary or secondary adapter.  D2D is also blocked when the
  // NV 310M is the primary adapter (using the standard blocklisting mechanism).
  // If the primary GPU already matched something in the blocklist then we
  // ignore this special rule.  See bug 1008759.
  if (status == nsIGfxInfo::FEATURE_STATUS_UNKNOWN &&
      (aFeature == nsIGfxInfo::FEATURE_DIRECT2D)) {
    if (!adapterInfoFailed[1]) {
      nsAString& nvVendorID =
          (nsAString&)GfxDriverInfo::GetDeviceVendor(DeviceVendor::NVIDIA);
      const nsString nv310mDeviceId = u"0x0A70"_ns;
      if (nvVendorID.Equals(adapterVendorID[1],
                            nsCaseInsensitiveStringComparator) &&
          nv310mDeviceId.Equals(adapterDeviceID[1],
                                nsCaseInsensitiveStringComparator)) {
        status = nsIGfxInfo::FEATURE_BLOCKED_DEVICE;
        aFailureId = "FEATURE_FAILURE_D2D_NV310M_BLOCK";
      }
    }
  }

  // Depends on Windows driver versioning. We don't pass a GfxDriverInfo object
  // back to the Windows handler, so we must handle this here.
  if (status == FEATURE_BLOCKED_DRIVER_VERSION) {
    if (info[i]->mSuggestedVersion) {
      aSuggestedVersion.AppendPrintf("%s", info[i]->mSuggestedVersion);
    } else if (info[i]->mComparisonOp == DRIVER_LESS_THAN &&
               info[i]->mDriverVersion != GfxDriverInfo::allDriverVersions) {
      aSuggestedVersion.AppendPrintf(
          "%lld.%lld.%lld.%lld",
          (info[i]->mDriverVersion & 0xffff000000000000) >> 48,
          (info[i]->mDriverVersion & 0x0000ffff00000000) >> 32,
          (info[i]->mDriverVersion & 0x00000000ffff0000) >> 16,
          (info[i]->mDriverVersion & 0x000000000000ffff));
    }
  }
#endif

  return status;
}

void GfxInfoBase::SetFeatureStatus(nsTArray<gfx::GfxInfoFeatureStatus>&& aFS) {
  MOZ_ASSERT(!sFeatureStatus);
  InitFeatureStatus(new nsTArray<gfx::GfxInfoFeatureStatus>(std::move(aFS)));
}

bool GfxInfoBase::DoesWindowProtocolMatch(
    const nsAString& aBlocklistWindowProtocol,
    const nsAString& aWindowProtocol) {
  return aBlocklistWindowProtocol.Equals(aWindowProtocol,
                                         nsCaseInsensitiveStringComparator) ||
         aBlocklistWindowProtocol.Equals(
             GfxDriverInfo::GetWindowProtocol(WindowProtocol::All),
             nsCaseInsensitiveStringComparator);
}

bool GfxInfoBase::DoesVendorMatch(const nsAString& aBlocklistVendor,
                                  const nsAString& aAdapterVendor) {
  return aBlocklistVendor.Equals(aAdapterVendor,
                                 nsCaseInsensitiveStringComparator) ||
         aBlocklistVendor.Equals(
             GfxDriverInfo::GetDeviceVendor(DeviceVendor::All),
             nsCaseInsensitiveStringComparator);
}

bool GfxInfoBase::DoesDriverVendorMatch(const nsAString& aBlocklistVendor,
                                        const nsAString& aDriverVendor) {
  return aBlocklistVendor.Equals(aDriverVendor,
                                 nsCaseInsensitiveStringComparator) ||
         aBlocklistVendor.Equals(
             GfxDriverInfo::GetDriverVendor(DriverVendor::All),
             nsCaseInsensitiveStringComparator);
}

bool GfxInfoBase::IsFeatureAllowlisted(int32_t aFeature) const {
  return aFeature == nsIGfxInfo::FEATURE_HW_DECODED_VIDEO_ZERO_COPY;
}

nsresult GfxInfoBase::GetFeatureStatusImpl(
    int32_t aFeature, int32_t* aStatus, nsAString& aSuggestedVersion,
    const nsTArray<RefPtr<GfxDriverInfo>>& aDriverInfo, nsACString& aFailureId,
    OperatingSystem* aOS /* = nullptr */) {
  if (aFeature <= 0) {
    gfxWarning() << "Invalid feature <= 0";
    return NS_OK;
  }

  if (*aStatus != nsIGfxInfo::FEATURE_STATUS_UNKNOWN) {
    // Terminate now with the status determined by the derived type (OS-specific
    // code).
    return NS_OK;
  }

  if (sShutdownOccurred) {
    // This is futile; we've already commenced shutdown and our blocklists have
    // been deleted. We may want to look into resurrecting the blocklist instead
    // but for now, just don't even go there.
    return NS_OK;
  }

  // Ensure any additional initialization required is complete.
  GetData();

  // If an operating system was provided by the derived GetFeatureStatusImpl,
  // grab it here. Otherwise, the OS is unknown.
  OperatingSystem os = (aOS ? *aOS : OperatingSystem::Unknown);

  nsAutoString adapterVendorID;
  nsAutoString adapterDeviceID;
  nsAutoString adapterDriverVersionString;
  if (NS_FAILED(GetAdapterVendorID(adapterVendorID)) ||
      NS_FAILED(GetAdapterDeviceID(adapterDeviceID)) ||
      NS_FAILED(GetAdapterDriverVersion(adapterDriverVersionString))) {
    if (OnlyAllowFeatureOnKnownConfig(aFeature)) {
      aFailureId = "FEATURE_FAILURE_CANT_RESOLVE_ADAPTER";
      *aStatus = nsIGfxInfo::FEATURE_BLOCKED_DEVICE;
    } else {
      *aStatus = nsIGfxInfo::FEATURE_STATUS_OK;
    }
    return NS_OK;
  }

  // We only check either the given blocklist, or the static list, as given.
  int32_t status;
  if (aDriverInfo.Length()) {
    status =
        FindBlocklistedDeviceInList(aDriverInfo, aSuggestedVersion, aFeature,
                                    aFailureId, os, /* aForAllowing */ false);
  } else {
    if (!sDriverInfo) {
      sDriverInfo = new nsTArray<RefPtr<GfxDriverInfo>>();
    }
    status = FindBlocklistedDeviceInList(GetGfxDriverInfo(), aSuggestedVersion,
                                         aFeature, aFailureId, os,
                                         /* aForAllowing */ false);
  }

  if (status == nsIGfxInfo::FEATURE_STATUS_UNKNOWN) {
    if (IsFeatureAllowlisted(aFeature)) {
      // This feature is actually using the allowlist; that means after we pass
      // the blocklist to prevent us explicitly from getting the feature, we now
      // need to check the allowlist to ensure we are allowed to get it in the
      // first place.
      if (aDriverInfo.Length()) {
        status = FindBlocklistedDeviceInList(aDriverInfo, aSuggestedVersion,
                                             aFeature, aFailureId, os,
                                             /* aForAllowing */ true);
      } else {
        status = FindBlocklistedDeviceInList(
            GetGfxDriverInfo(), aSuggestedVersion, aFeature, aFailureId, os,
            /* aForAllowing */ true);
      }

      if (status == nsIGfxInfo::FEATURE_STATUS_UNKNOWN) {
        status = nsIGfxInfo::FEATURE_DENIED;
      }
    } else {
      // It's now done being processed. It's safe to set the status to
      // STATUS_OK.
      status = nsIGfxInfo::FEATURE_STATUS_OK;
    }
  }

  *aStatus = status;
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetFeatureSuggestedDriverVersion(int32_t aFeature,
                                              nsAString& aVersion) {
  nsCString version;
  if (GetPrefValueForDriverVersion(version)) {
    aVersion = NS_ConvertASCIItoUTF16(version);
    return NS_OK;
  }

  int32_t status;
  nsCString discardFailureId;
  nsTArray<RefPtr<GfxDriverInfo>> driverInfo;
  return GetFeatureStatusImpl(aFeature, &status, aVersion, driverInfo,
                              discardFailureId);
}

NS_IMETHODIMP
GfxInfoBase::GetFeatureSuggestedDriverVersionStr(const nsAString& aFeature,
                                                 nsAString& aVersion) {
  int32_t feature = BlocklistFeatureToGfxFeature(aFeature);
  if (feature == nsIGfxInfo::FEATURE_INVALID) {
    NS_ConvertUTF16toUTF8 feature(aFeature);
    gfxWarning() << "Unrecognized feature " << feature.get();
    return NS_ERROR_INVALID_ARG;
  }
  return GetFeatureSuggestedDriverVersion(feature, aVersion);
}

void GfxInfoBase::EvaluateDownloadedBlocklist(
    nsTArray<RefPtr<GfxDriverInfo>>& aDriverInfo) {
  // If the list is empty, then we don't actually want to call
  // GetFeatureStatusImpl since we will use the static list instead. In that
  // case, all we want to do is make sure the pref is removed.
  if (aDriverInfo.IsEmpty()) {
    gfxCriticalNoteOnce << "Evaluate empty downloaded blocklist";
    return;
  }

  OperatingSystem os = GetOperatingSystem();

  // For every feature we know about, we evaluate whether this blocklist has a
  // non-STATUS_OK status. If it does, we set the pref we evaluate in
  // GetFeatureStatus above, so we don't need to hold on to this blocklist
  // anywhere permanent.
  for (int feature = nsIGfxInfo::FEATURE_START;
       feature < nsIGfxInfo::FEATURE_COUNT; ++feature) {
    int32_t status = nsIGfxInfo::FEATURE_STATUS_UNKNOWN;
    nsCString failureId;
    nsAutoString suggestedVersion;

    // Note that we are careful to call the base class method since we only want
    // to evaluate the downloadable blocklist for these prefs.
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(GfxInfoBase::GetFeatureStatusImpl(
        feature, &status, suggestedVersion, aDriverInfo, failureId, &os)));

    switch (status) {
      default:
        MOZ_FALLTHROUGH_ASSERT("Unhandled feature status!");
      case nsIGfxInfo::FEATURE_STATUS_UNKNOWN:
        // This may be returned during shutdown or for invalid features.
      case nsIGfxInfo::FEATURE_ALLOW_ALWAYS:
      case nsIGfxInfo::FEATURE_ALLOW_QUALIFIED:
      case nsIGfxInfo::FEATURE_DENIED:
        // We cannot use the downloadable blocklist to control the allowlist.
        // If a feature is allowlisted, then we should also ignore DENIED
        // statuses from GetFeatureStatusImpl because we don't check the
        // static list when and this is an expected value. If we wish to
        // override the allowlist, it is as simple as creating a normal
        // blocklist rule with a BLOCKED* status code.
      case nsIGfxInfo::FEATURE_STATUS_OK:
        RemovePrefForFeature(feature);
        break;

      case nsIGfxInfo::FEATURE_BLOCKED_DRIVER_VERSION:
        if (!suggestedVersion.IsEmpty()) {
          SetPrefValueForDriverVersion(suggestedVersion);
        } else {
          RemovePrefForDriverVersion();
        }
        [[fallthrough]];

      case nsIGfxInfo::FEATURE_BLOCKED_MISMATCHED_VERSION:
      case nsIGfxInfo::FEATURE_BLOCKED_DEVICE:
      case nsIGfxInfo::FEATURE_DISCOURAGED:
      case nsIGfxInfo::FEATURE_BLOCKED_OS_VERSION:
      case nsIGfxInfo::FEATURE_BLOCKED_PLATFORM_TEST:
        SetPrefValueForFeature(feature, status, failureId);
        break;
    }
  }
}

NS_IMETHODIMP_(void)
GfxInfoBase::LogFailure(const nsACString& failure) {
  // gfxCriticalError has a mutex lock of its own, so we may not actually
  // need this lock. ::GetFailures() accesses the data but the LogForwarder
  // will not return the copy of the logs unless it can get the same lock
  // that gfxCriticalError uses.  Still, that is so much of an implementation
  // detail that it's nicer to just add an extra lock here and in
  // ::GetFailures()
  MutexAutoLock lock(mMutex);

  // By default, gfxCriticalError asserts; make it not assert in this case.
  gfxCriticalError(CriticalLog::DefaultOptions(false))
      << "(LF) " << failure.BeginReading();
}

NS_IMETHODIMP GfxInfoBase::GetFailures(nsTArray<int32_t>& indices,
                                       nsTArray<nsCString>& failures) {
  MutexAutoLock lock(mMutex);

  LogForwarder* logForwarder = Factory::GetLogForwarder();
  if (!logForwarder) {
    return NS_ERROR_UNEXPECTED;
  }

  // There are two string copies in this method, starting with this one. We are
  // assuming this is not a big deal, as the size of the array should be small
  // and the strings in it should be small as well (the error messages in the
  // code.)  The second copy happens with the AppendElement() calls.
  // Technically, we don't need the mutex lock after the StringVectorCopy()
  // call.
  LoggingRecord loggedStrings = logForwarder->LoggingRecordCopy();
  LoggingRecord::const_iterator it;
  for (it = loggedStrings.begin(); it != loggedStrings.end(); ++it) {
    failures.AppendElement(nsDependentCSubstring(std::get<1>(*it).c_str(),
                                                 std::get<1>(*it).size()));
    indices.AppendElement(std::get<0>(*it));
  }

  return NS_OK;
}

nsTArray<GfxInfoCollectorBase*>* sCollectors;

static void InitCollectors() {
  if (!sCollectors) sCollectors = new nsTArray<GfxInfoCollectorBase*>;
}

nsresult GfxInfoBase::GetInfo(JSContext* aCx,
                              JS::MutableHandle<JS::Value> aResult) {
  InitCollectors();
  InfoObject obj(aCx);

  for (uint32_t i = 0; i < sCollectors->Length(); i++) {
    (*sCollectors)[i]->GetInfo(obj);
  }

  // Some example property definitions
  // obj.DefineProperty("wordCacheSize", gfxTextRunWordCache::Count());
  // obj.DefineProperty("renderer", mRendererIDsString);
  // obj.DefineProperty("five", 5);

  if (!obj.mOk) {
    return NS_ERROR_FAILURE;
  }

  aResult.setObject(*obj.mObj);
  return NS_OK;
}

MOZ_RUNINIT nsAutoCString gBaseAppVersion;

const nsCString& GfxInfoBase::GetApplicationVersion() {
  static bool versionInitialized = false;
  if (!versionInitialized) {
    // If we fail to get the version, we will not try again.
    versionInitialized = true;

    // Get the version from xpcom/system/nsIXULAppInfo.idl
    nsCOMPtr<nsIXULAppInfo> app = do_GetService("@mozilla.org/xre/app-info;1");
    if (app) {
      app->GetVersion(gBaseAppVersion);
    }
  }
  return gBaseAppVersion;
}

/* static */ bool GfxInfoBase::OnlyAllowFeatureOnKnownConfig(int32_t aFeature) {
  switch (aFeature) {
    // The GPU process doesn't need hardware acceleration and can run on
    // devices that we normally block from not being on our whitelist.
    case nsIGfxInfo::FEATURE_GPU_PROCESS:
      return kIsAndroid;
    // We can mostly assume that ANGLE will work
    case nsIGfxInfo::FEATURE_DIRECT3D_11_ANGLE:
    // Remote WebGL is needed for Win32k Lockdown, so it should be enabled
    // regardless of HW support or not
    case nsIGfxInfo::FEATURE_ALLOW_WEBGL_OUT_OF_PROCESS:
    // Backdrop filter should generally work, especially if we fall back to
    // Software WebRender because of an unknown vendor.
    case nsIGfxInfo::FEATURE_BACKDROP_FILTER:
      return false;
    default:
      return true;
  }
}

void GfxInfoBase::AddCollector(GfxInfoCollectorBase* collector) {
  InitCollectors();
  sCollectors->AppendElement(collector);
}

void GfxInfoBase::RemoveCollector(GfxInfoCollectorBase* collector) {
  InitCollectors();
  for (uint32_t i = 0; i < sCollectors->Length(); i++) {
    if ((*sCollectors)[i] == collector) {
      sCollectors->RemoveElementAt(i);
      break;
    }
  }
  if (sCollectors->IsEmpty()) {
    delete sCollectors;
    sCollectors = nullptr;
  }
}

static void AppendMonitor(JSContext* aCx, widget::Screen& aScreen,
                          JS::Handle<JSObject*> aOutArray, int32_t aIndex) {
  JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));

  auto screenSize = aScreen.GetRect().Size();

  JS::Rooted<JS::Value> screenWidth(aCx, JS::Int32Value(screenSize.width));
  JS_SetProperty(aCx, obj, "screenWidth", screenWidth);

  JS::Rooted<JS::Value> screenHeight(aCx, JS::Int32Value(screenSize.height));
  JS_SetProperty(aCx, obj, "screenHeight", screenHeight);

  JS::Rooted<JS::Value> defaultCssScaleFactor(
      aCx,
      JS::Float32Value(static_cast<float>(aScreen.GetDefaultCSSScaleFactor())));
  JS_SetProperty(aCx, obj, "defaultCSSScaleFactor", defaultCssScaleFactor);

  JS::Rooted<JS::Value> contentsScaleFactor(
      aCx, JS::NumberValue(aScreen.GetContentsScaleFactor()));
  JS_SetProperty(aCx, obj, "contentsScaleFactor", contentsScaleFactor);

#ifdef XP_WIN
  JS::Rooted<JS::Value> refreshRate(aCx,
                                    JS::Int32Value(aScreen.GetRefreshRate()));
  JS_SetProperty(aCx, obj, "refreshRate", refreshRate);

  JS::Rooted<JS::Value> pseudoDisplay(
      aCx, JS::BooleanValue(aScreen.GetIsPseudoDisplay()));
  JS_SetProperty(aCx, obj, "pseudoDisplay", pseudoDisplay);
#endif

  JS::Rooted<JS::Value> element(aCx, JS::ObjectValue(*obj));
  JS_SetElement(aCx, aOutArray, aIndex, element);
}

nsresult GfxInfoBase::FindMonitors(JSContext* aCx,
                                   JS::Handle<JSObject*> aOutArray) {
  int32_t index = 0;
  auto& sm = ScreenManager::GetSingleton();
  for (auto& screen : sm.CurrentScreenList()) {
    AppendMonitor(aCx, *screen, aOutArray, index++);
  }

  if (index == 0) {
    // Ensure we return at least one monitor, this is needed for xpcshell.
    RefPtr<Screen> screen = sm.GetPrimaryScreen();
    AppendMonitor(aCx, *screen, aOutArray, index++);
  }

  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetMonitors(JSContext* aCx, JS::MutableHandle<JS::Value> aResult) {
  JS::Rooted<JSObject*> array(aCx, JS::NewArrayObject(aCx, 0));

  nsresult rv = FindMonitors(aCx, array);
  if (NS_FAILED(rv)) {
    return rv;
  }

  aResult.setObject(*array);
  return NS_OK;
}

static inline bool SetJSPropertyString(JSContext* aCx,
                                       JS::Handle<JSObject*> aObj,
                                       const char* aProp, const char* aString) {
  JS::Rooted<JSString*> str(aCx, JS_NewStringCopyZ(aCx, aString));
  if (!str) {
    return false;
  }

  JS::Rooted<JS::Value> val(aCx, JS::StringValue(str));
  return JS_SetProperty(aCx, aObj, aProp, val);
}

template <typename T>
static inline bool AppendJSElement(JSContext* aCx, JS::Handle<JSObject*> aObj,
                                   const T& aValue) {
  uint32_t index;
  if (!JS::GetArrayLength(aCx, aObj, &index)) {
    return false;
  }
  return JS_SetElement(aCx, aObj, index, aValue);
}

nsresult GfxInfoBase::GetFeatures(JSContext* aCx,
                                  JS::MutableHandle<JS::Value> aOut) {
  JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));
  if (!obj) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  aOut.setObject(*obj);

  layers::LayersBackend backend =
      gfxPlatform::Initialized()
          ? gfxPlatform::GetPlatform()->GetCompositorBackend()
          : layers::LayersBackend::LAYERS_NONE;
  const char* backendName = layers::GetLayersBackendName(backend);
  SetJSPropertyString(aCx, obj, "compositor", backendName);

  // If graphics isn't initialized yet, just stop now.
  if (!gfxPlatform::Initialized()) {
    return NS_OK;
  }

  DescribeFeatures(aCx, obj);
  return NS_OK;
}

nsresult GfxInfoBase::GetFeatureLog(JSContext* aCx,
                                    JS::MutableHandle<JS::Value> aOut) {
  JS::Rooted<JSObject*> containerObj(aCx, JS_NewPlainObject(aCx));
  if (!containerObj) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  aOut.setObject(*containerObj);

  JS::Rooted<JSObject*> featureArray(aCx, JS::NewArrayObject(aCx, 0));
  if (!featureArray) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  // Collect features.
  gfxConfig::ForEachFeature([&](const char* aName, const char* aDescription,
                                FeatureState& aFeature) -> void {
    JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));
    if (!obj) {
      return;
    }
    if (!SetJSPropertyString(aCx, obj, "name", aName) ||
        !SetJSPropertyString(aCx, obj, "description", aDescription) ||
        !SetJSPropertyString(aCx, obj, "status",
                             FeatureStatusToString(aFeature.GetValue()))) {
      return;
    }

    JS::Rooted<JS::Value> log(aCx);
    if (!BuildFeatureStateLog(aCx, aFeature, &log)) {
      return;
    }
    if (!JS_SetProperty(aCx, obj, "log", log)) {
      return;
    }

    if (!AppendJSElement(aCx, featureArray, obj)) {
      return;
    }
  });

  JS::Rooted<JSObject*> fallbackArray(aCx, JS::NewArrayObject(aCx, 0));
  if (!fallbackArray) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  // Collect fallbacks.
  gfxConfig::ForEachFallback(
      [&](const char* aName, const char* aMessage) -> void {
        JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));
        if (!obj) {
          return;
        }

        if (!SetJSPropertyString(aCx, obj, "name", aName) ||
            !SetJSPropertyString(aCx, obj, "message", aMessage)) {
          return;
        }

        if (!AppendJSElement(aCx, fallbackArray, obj)) {
          return;
        }
      });

  JS::Rooted<JS::Value> val(aCx);

  val = JS::ObjectValue(*featureArray);
  JS_SetProperty(aCx, containerObj, "features", val);

  val = JS::ObjectValue(*fallbackArray);
  JS_SetProperty(aCx, containerObj, "fallbacks", val);

  return NS_OK;
}

bool GfxInfoBase::BuildFeatureStateLog(JSContext* aCx,
                                       const FeatureState& aFeature,
                                       JS::MutableHandle<JS::Value> aOut) {
  JS::Rooted<JSObject*> log(aCx, JS::NewArrayObject(aCx, 0));
  if (!log) {
    return false;
  }
  aOut.setObject(*log);

  aFeature.ForEachStatusChange([&](const char* aType, FeatureStatus aStatus,
                                   const char* aMessage,
                                   const nsCString& aFailureId) -> void {
    JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));
    if (!obj) {
      return;
    }

    if (!SetJSPropertyString(aCx, obj, "type", aType) ||
        !SetJSPropertyString(aCx, obj, "status",
                             FeatureStatusToString(aStatus)) ||
        (!aFailureId.IsEmpty() &&
         !SetJSPropertyString(aCx, obj, "failureId", aFailureId.get())) ||
        (aMessage && !SetJSPropertyString(aCx, obj, "message", aMessage))) {
      return;
    }

    if (!AppendJSElement(aCx, log, obj)) {
      return;
    }
  });

  return true;
}

void GfxInfoBase::DescribeFeatures(JSContext* aCx, JS::Handle<JSObject*> aObj) {
  JS::Rooted<JSObject*> obj(aCx);

  gfx::FeatureState& hwCompositing =
      gfxConfig::GetFeature(gfx::Feature::HW_COMPOSITING);
  InitFeatureObject(aCx, aObj, "hwCompositing", hwCompositing, &obj);

  gfx::FeatureState& gpuProcess =
      gfxConfig::GetFeature(gfx::Feature::GPU_PROCESS);
  InitFeatureObject(aCx, aObj, "gpuProcess", gpuProcess, &obj);

  gfx::FeatureState& webrender = gfxConfig::GetFeature(gfx::Feature::WEBRENDER);
  InitFeatureObject(aCx, aObj, "webrender", webrender, &obj);

  gfx::FeatureState& wrCompositor =
      gfxConfig::GetFeature(gfx::Feature::WEBRENDER_COMPOSITOR);
  InitFeatureObject(aCx, aObj, "wrCompositor", wrCompositor, &obj);

  gfx::FeatureState& openglCompositing =
      gfxConfig::GetFeature(gfx::Feature::OPENGL_COMPOSITING);
  InitFeatureObject(aCx, aObj, "openglCompositing", openglCompositing, &obj);

  gfx::FeatureState& omtp = gfxConfig::GetFeature(gfx::Feature::OMTP);
  InitFeatureObject(aCx, aObj, "omtp", omtp, &obj);
}

bool GfxInfoBase::InitFeatureObject(JSContext* aCx,
                                    JS::Handle<JSObject*> aContainer,
                                    const char* aName,
                                    mozilla::gfx::FeatureState& aFeatureState,
                                    JS::MutableHandle<JSObject*> aOutObj) {
  JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));
  if (!obj) {
    return false;
  }

  nsCString status = aFeatureState.GetStatusAndFailureIdString();

  JS::Rooted<JSString*> str(aCx, JS_NewStringCopyZ(aCx, status.get()));
  JS::Rooted<JS::Value> val(aCx, JS::StringValue(str));
  JS_SetProperty(aCx, obj, "status", val);

  // Add the feature object to the container.
  {
    JS::Rooted<JS::Value> val(aCx, JS::ObjectValue(*obj));
    JS_SetProperty(aCx, aContainer, aName, val);
  }

  aOutObj.set(obj);
  return true;
}

nsresult GfxInfoBase::GetActiveCrashGuards(JSContext* aCx,
                                           JS::MutableHandle<JS::Value> aOut) {
  JS::Rooted<JSObject*> array(aCx, JS::NewArrayObject(aCx, 0));
  if (!array) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  aOut.setObject(*array);

  DriverCrashGuard::ForEachActiveCrashGuard(
      [&](const char* aName, const char* aPrefName) -> void {
        JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));
        if (!obj) {
          return;
        }
        if (!SetJSPropertyString(aCx, obj, "type", aName)) {
          return;
        }
        if (!SetJSPropertyString(aCx, obj, "prefName", aPrefName)) {
          return;
        }
        if (!AppendJSElement(aCx, array, obj)) {
          return;
        }
      });

  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetTargetFrameRate(uint32_t* aTargetFrameRate) {
  *aTargetFrameRate = gfxPlatform::TargetFrameRate();
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetCodecSupportInfo(nsACString& aCodecSupportInfo) {
  aCodecSupportInfo.Assign(gfx::gfxVars::CodecSupportInfo());
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetIsHeadless(bool* aIsHeadless) {
  *aIsHeadless = gfxPlatform::IsHeadless();
  return NS_OK;
}

#if defined(MOZ_WIDGET_ANDROID)

const char* chromebookProductList[] = {
    "asuka",    "asurada",   "atlas",    "auron",    "banjo",     "banon",
    "bob",      "brask",     "brya",     "buddy",    "butterfly", "candy",
    "caroline", "cave",      "celes",    "chell",    "cherry",    "clapper",
    "coral",    "corsola",   "cyan",     "daisy",    "dedede",    "drallion",
    "edgar",    "elm",       "enguarde", "eve",      "expresso",  "falco",
    "fizz",     "gandof",    "glimmer",  "gnawty",   "grunt",     "guado",
    "guybrush", "hana",      "hatch",    "heli",     "jacuzzi",   "kalista",
    "kefka",    "kevin",     "kip",      "kukui",    "lars",      "leon",
    "link",     "lulu",      "lumpy",    "mccloud",  "monroe",    "nami",
    "nautilus", "ninja",     "nissa",    "nocturne", "nyan",      "octopus",
    "orco",     "panther",   "parrot",   "peach",    "peppy",     "puff",
    "pyro",     "quawks",    "rammus",   "reef",     "reks",      "relm",
    "rikku",    "samus",     "sand",     "sarien",   "scarlet",   "sentry",
    "setzer",   "skyrim",    "snappy",   "soraka",   "squawks",   "staryu",
    "stout",    "strongbad", "stumpy",   "sumo",     "swanky",    "terra",
    "tidus",    "tricky",    "trogdor",  "ultima",   "veyron",    "volteer",
    "winky",    "wizpig",    "wolf",     "x86",      "zako",      "zork"};

bool ProductIsChromebook(nsCString product) {
  size_t result;
  return BinarySearchIf(
      chromebookProductList, 0, std::size(chromebookProductList),
      [&](const char* const aValue) -> int {
        return strcmp(product.get(), aValue);
      },
      &result);
}
#endif

using Device = nsIGfxInfo::FontVisibilityDeviceDetermination;
static StaticAutoPtr<std::pair<Device, nsString>> ret;

std::pair<Device, nsString>* GfxInfoBase::GetFontVisibilityDeterminationPair() {
  if (!ret) {
    ret = new std::pair<Device, nsString>();
    ret->first = Device::Unassigned;
    ret->second = u""_ns;
    ClearOnShutdown(&ret);
  }

  if (ret->first != Device::Unassigned) {
    return ret;
  }

#if defined(MOZ_WIDGET_ANDROID)
  auto androidReleaseVersion = strtol(
      java::sdk::Build::VERSION::RELEASE()->ToCString().get(), nullptr, 10);

  auto androidManufacturer = java::sdk::Build::MANUFACTURER()->ToCString();
  nsContentUtils::ASCIIToLower(androidManufacturer);

  auto androidBrand = java::sdk::Build::BRAND()->ToCString();
  nsContentUtils::ASCIIToLower(androidBrand);

  auto androidModel = java::sdk::Build::MODEL()->ToCString();
  nsContentUtils::ASCIIToLower(androidModel);

  auto androidProduct = java::sdk::Build::PRODUCT()->ToCString();
  nsContentUtils::ASCIIToLower(androidProduct);

  auto androidProductIsChromebook = ProductIsChromebook(androidProduct);

  if (androidReleaseVersion < 4 || androidReleaseVersion > 20) {
    // Something is screwy, oh well.
    ret->second.AppendASCII("Unknown Release Version - ");
    ret->first = Device::Android_Unknown_Release_Version;
  } else if (androidReleaseVersion <= 8) {
    ret->second.AppendASCII("Android <9 - ");
    ret->first = Device::Android_sub_9;
  } else if (androidReleaseVersion <= 11) {
    ret->second.AppendASCII("Android 9-11 - ");
    ret->first = Device::Android_9_11;
  } else if (androidReleaseVersion > 11) {
    ret->second.AppendASCII("Android 12+ - ");
    ret->first = Device::Android_12_plus;
  } else {
    MOZ_CRASH_UNSAFE_PRINTF(
        "Somehow wound up in GetFontVisibilityDeterminationPair with a release "
        "version of %li",
        androidReleaseVersion);
  }

  if (androidManufacturer == "google" && androidModel == androidProduct &&
      androidProductIsChromebook) {
    // Chromebook font set coming later
    ret->second.AppendASCII("Chromebook - ");
    ret->first = Device::Android_Chromebook;
  }
  if (androidBrand == "amazon") {
    // Amazon Fire font set coming later
    ret->second.AppendASCII("Amazon - ");
    ret->first = Device::Android_Amazon;
  }
  if (androidBrand == "peloton") {
    // We don't know how to categorize fonts on this system
    ret->second.AppendASCII("Peloton - ");
    ret->first = Device::Android_Unknown_Peloton;
  }
  if (androidProduct == "vbox86p") {
    ret->second.AppendASCII("vbox - ");
    // We can't categorize fonts when running in an emulator on a Desktop
    ret->first = Device::Android_Unknown_vbox;
  }
  if (androidModel.Find("mitv"_ns) != kNotFound && androidBrand == "xiaomi") {
    // We don't know how to categorize fonts on this system
    ret->second.AppendASCII("mitv - ");
    ret->first = Device::Android_Unknown_mitv;
  }

  ret->second.AppendPrintf(
      "release_version_str=%s, release_version=%li",
      java::sdk::Build::VERSION::RELEASE()->ToCString().get(),
      androidReleaseVersion);
  ret->second.AppendPrintf(
      ", manufacturer=%s, brand=%s, model=%s, product=%s, chromebook=%s",
      androidManufacturer.get(), androidBrand.get(), androidModel.get(),
      androidProduct.get(), androidProductIsChromebook ? "yes" : "no");

#elif defined(XP_LINUX)
  ret->first = Device::Linux_Unknown;

  long versionMajor = 0;
  FILE* fp = fopen("/etc/os-release", "r");
  if (fp) {
    char buf[512];
    while (fgets(buf, sizeof(buf), fp)) {
      if (strncmp(buf, "VERSION_ID=\"", 12) == 0) {
        ret->second.AppendPrintf("VERSION_ID=%.11s", buf + 11);
        versionMajor = strtol(buf + 12, nullptr, 10);
        if (ret->first != Device::Linux_Unknown) {
          break;
        }
      }

      if (strncmp(buf, "ID=", 3) == 0) {
        ret->second.AppendPrintf("ID=%.6s", buf + 3);
        if (strncmp(buf + 3, "ubuntu", 6) == 0) {
          ret->first = Device::Linux_Ubuntu_any;
        } else if (strncmp(buf + 3, "fedora", 6) == 0) {
          ret->first = Device::Linux_Fedora_any;
        }

        if (versionMajor) {
          break;
        }
      }
    }
    fclose(fp);
  }
  if (ret->first == Device::Linux_Ubuntu_any) {
    if (versionMajor == 20) {
      ret->first = Device::Linux_Ubuntu_20;
      ret->second.Insert(u"Ubuntu 20 - ", 0);
    } else if (versionMajor == 22) {
      ret->first = Device::Linux_Ubuntu_22;
      ret->second.Insert(u"Ubuntu 22 - ", 0);
    } else {
      ret->second.Insert(u"Ubuntu Unknown - ", 0);
    }
  } else if (ret->first == Device::Linux_Fedora_any) {
    if (versionMajor == 38) {
      ret->first = Device::Linux_Fedora_38;
      ret->second.Insert(u"Fedora 38 - ", 0);
    } else if (versionMajor == 39) {
      ret->first = Device::Linux_Fedora_39;
      ret->second.Insert(u"Fedora 39 - ", 0);
    } else {
      ret->second.Insert(u"Fedora Unknown - ", 0);
    }
  } else {
    ret->second.Insert(u"Linux Unknown - ", 0);
  }

#elif defined(XP_MACOSX)
  ret->first = Device::MacOS_Unknown;
  ret->second.AppendASCII("macOS Platform");

  int major = 0;
  int minor = 0;
  int bugfix = 0;
  nsCocoaFeatures::GetSystemVersion(major, minor, bugfix);
  if (major == 0) {
    return ret;
  }

  ret->first = major >= 13 ? Device::MacOS_13_plus : Device::MacOS_sub_13;
  ret->second.AppendPrintf("macOS %d.%d.%d", major, minor, bugfix);
#elif defined(XP_WIN)
  ret->first = Device::Windows_Platform;
  ret->second.AppendASCII("Windows Platform");
#else
  ret->first = Device::Unknown_Platform;
  ret->second.AppendASCII("Unknown Platform");
#endif

  return ret;
}

NS_IMETHODIMP
GfxInfoBase::GetFontVisibilityDetermination(
    Device* aFontVisibilityDetermination) {
  auto ret = GetFontVisibilityDeterminationPair();

  *aFontVisibilityDetermination = ret->first;
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetFontVisibilityDeterminationStr(
    nsAString& aFontVisibilityDeterminationStr) {
  auto ret = GetFontVisibilityDeterminationPair();
  aFontVisibilityDeterminationStr.Assign(ret->second);
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetContentBackend(nsAString& aContentBackend) {
  BackendType backend = gfxPlatform::GetPlatform()->GetDefaultContentBackend();
  nsString outStr;

  switch (backend) {
    case BackendType::DIRECT2D1_1: {
      outStr.AppendPrintf("Direct2D 1.1");
      break;
    }
    case BackendType::SKIA: {
      outStr.AppendPrintf("Skia");
      break;
    }
    case BackendType::CAIRO: {
      outStr.AppendPrintf("Cairo");
      break;
    }
    default:
      return NS_ERROR_FAILURE;
  }

  aContentBackend.Assign(outStr);
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetAzureCanvasBackend(nsAString& aBackend) {
  CopyASCIItoUTF16(mozilla::MakeStringSpan(
                       gfxPlatform::GetPlatform()->GetAzureCanvasBackend()),
                   aBackend);
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetAzureContentBackend(nsAString& aBackend) {
  CopyASCIItoUTF16(mozilla::MakeStringSpan(
                       gfxPlatform::GetPlatform()->GetAzureContentBackend()),
                   aBackend);
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetUsingGPUProcess(bool* aOutValue) {
  GPUProcessManager* gpu = GPUProcessManager::Get();
  if (!gpu) {
    // Not supported in content processes.
    return NS_ERROR_FAILURE;
  }

  *aOutValue = !!gpu->GetGPUChild();
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetUsingRemoteCanvas(bool* aOutValue) {
  *aOutValue = gfx::gfxVars::RemoteCanvasEnabled();
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetUsingAcceleratedCanvas(bool* aOutValue) {
  *aOutValue = gfx::gfxVars::UseAcceleratedCanvas2D();
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::GetTextScaleFactor(float* aOutValue) {
  *aOutValue = LookAndFeel::GetTextScaleFactor();
  return NS_OK;
}

NS_IMETHODIMP
GfxInfoBase::ControlGPUProcessForXPCShell(bool aEnable, bool* _retval) {
  gfxPlatform::GetPlatform();

  GPUProcessManager* gpm = GPUProcessManager::Get();
  if (aEnable) {
    if (!gfxConfig::IsEnabled(gfx::Feature::GPU_PROCESS)) {
      gfxConfig::UserForceEnable(gfx::Feature::GPU_PROCESS, "xpcshell-test");
    }
    DebugOnly<nsresult> rv = gpm->EnsureGPUReady();
    MOZ_ASSERT(rv != NS_ERROR_ILLEGAL_DURING_SHUTDOWN);
  } else {
    gfxConfig::UserDisable(gfx::Feature::GPU_PROCESS, "xpcshell-test");
    gpm->KillProcess();
  }

  *_retval = true;
  return NS_OK;
}

NS_IMETHODIMP GfxInfoBase::KillGPUProcessForTests() {
  GPUProcessManager* gpm = GPUProcessManager::Get();
  if (!gpm) {
    // gfxPlatform has not been initialized.
    return NS_ERROR_NOT_INITIALIZED;
  }

  gpm->KillProcess();
  return NS_OK;
}

NS_IMETHODIMP GfxInfoBase::CrashGPUProcessForTests() {
  GPUProcessManager* gpm = GPUProcessManager::Get();
  if (!gpm) {
    // gfxPlatform has not been initialized.
    return NS_ERROR_NOT_INITIALIZED;
  }

  gpm->CrashProcess();
  return NS_OK;
}

GfxInfoCollectorBase::GfxInfoCollectorBase() {
  GfxInfoBase::AddCollector(this);
}

GfxInfoCollectorBase::~GfxInfoCollectorBase() {
  GfxInfoBase::RemoveCollector(this);
}
