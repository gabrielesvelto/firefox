/* vim: se cin sw=2 ts=2 et : */
/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __mozilla_widget_GfxInfoBase_h__
#define __mozilla_widget_GfxInfoBase_h__

#include "GfxDriverInfo.h"
#include "GfxInfoCollector.h"
#include "gfxFeature.h"
#include "gfxTelemetry.h"
#include "js/Value.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"
#include "mozilla/Mutex.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/gfx/GraphicsMessages.h"
#include "nsCOMPtr.h"
#include "nsIGfxInfo.h"
#include "nsIGfxInfoDebug.h"
#include "nsIObserver.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsWeakReference.h"

namespace mozilla {
namespace widget {

class GfxInfoBase : public nsIGfxInfo,
                    public nsIObserver,
                    public nsSupportsWeakReference
#ifdef DEBUG
    ,
                    public nsIGfxInfoDebug
#endif
{
 public:
  GfxInfoBase();

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIOBSERVER

  // We only declare a subset of the nsIGfxInfo interface. It's up to derived
  // classes to implement the rest of the interface.
  // Derived classes need to use
  // using GfxInfoBase::GetFeatureStatus;
  // using GfxInfoBase::GetFeatureSuggestedDriverVersion;
  // to import the relevant methods into their namespace.
  NS_IMETHOD GetFeatureStatus(int32_t aFeature, nsACString& aFailureId,
                              int32_t* _retval) override;
  NS_IMETHOD GetFeatureSuggestedDriverVersion(int32_t aFeature,
                                              nsAString& _retval) override;
  NS_IMETHOD GetFeatureStatusStr(const nsAString& aFeature,
                                 nsACString& aFailureId,
                                 nsAString& _retval) override;
  NS_IMETHOD GetFeatureSuggestedDriverVersionStr(const nsAString& aFeature,
                                                 nsAString& _retval) override;

  NS_IMETHOD GetMonitors(JSContext* cx,
                         JS::MutableHandle<JS::Value> _retval) override;
  NS_IMETHOD GetFailures(nsTArray<int32_t>& indices,
                         nsTArray<nsCString>& failures) override;
  NS_IMETHOD_(void) LogFailure(const nsACString& failure) override;
  NS_IMETHOD GetInfo(JSContext*, JS::MutableHandle<JS::Value>) override;
  NS_IMETHOD GetFeatures(JSContext*, JS::MutableHandle<JS::Value>) override;
  NS_IMETHOD GetFeatureLog(JSContext*, JS::MutableHandle<JS::Value>) override;
  NS_IMETHOD GetActiveCrashGuards(JSContext*,
                                  JS::MutableHandle<JS::Value>) override;
  NS_IMETHOD GetFontVisibilityDetermination(
      nsIGfxInfo::FontVisibilityDeviceDetermination*
          aFontVisibilityDetermination) override;
  NS_IMETHOD GetFontVisibilityDeterminationStr(
      nsAString& aFontVisibilityDeterminationStr) override;
  NS_IMETHOD GetContentBackend(nsAString& aContentBackend) override;
  NS_IMETHOD GetAzureCanvasBackend(nsAString& aBackend) override;
  NS_IMETHOD GetAzureContentBackend(nsAString& aBackend) override;
  NS_IMETHOD GetUsingGPUProcess(bool* aOutValue) override;
  NS_IMETHOD GetUsingRemoteCanvas(bool* aOutValue) override;
  NS_IMETHOD GetUsingAcceleratedCanvas(bool* aOutValue) override;
  NS_IMETHOD GetIsHeadless(bool* aIsHeadless) override;
  NS_IMETHOD GetTargetFrameRate(uint32_t* aTargetFrameRate) override;
  NS_IMETHOD GetCodecSupportInfo(nsACString& aCodecSupportInfo) override;

#ifdef DEBUG
  NS_IMETHOD SpoofMonitorInfo(uint32_t aScreenCount, int32_t aMinRefreshRate,
                              int32_t aMaxRefreshRate) override;
#endif

  // Non-XPCOM method to get IPC data:
  nsTArray<mozilla::gfx::GfxInfoFeatureStatus> GetAllFeatures();

  // Initialization function. If you override this, you must call this class's
  // version of Init first.
  // We need Init to be called separately from the constructor so we can
  // register as an observer after all derived classes have been constructed
  // and we know we have a non-zero refcount.
  // Ideally, Init() would be void-return, but the rules of
  // NS_GENERIC_FACTORY_CONSTRUCTOR_INIT require it be nsresult return.
  virtual nsresult Init();

  NS_IMETHOD_(void) GetData() override;
  NS_IMETHOD GetTextScaleFactor(float* aOutValue) override;

  static void AddCollector(GfxInfoCollectorBase* collector);
  static void RemoveCollector(GfxInfoCollectorBase* collector);

  static StaticAutoPtr<nsTArray<RefPtr<GfxDriverInfo>>> sDriverInfo;
  static StaticAutoPtr<nsTArray<mozilla::gfx::GfxInfoFeatureStatus>>
      sFeatureStatus;
  static bool sDriverInfoObserverInitialized;
  static bool sShutdownOccurred;

  virtual nsString Model() { return u""_ns; }
  virtual nsString Hardware() { return u""_ns; }
  virtual nsString Product() { return u""_ns; }
  virtual nsString Manufacturer() { return u""_ns; }
  virtual uint32_t OperatingSystemVersion() { return 0; }
  virtual GfxVersionEx OperatingSystemVersionEx() { return GfxVersionEx(); }

  // Convenience to get the application version
  static const nsCString& GetApplicationVersion();

  virtual nsresult FindMonitors(JSContext* cx, JS::Handle<JSObject*> array);

  static void SetFeatureStatus(
      nsTArray<mozilla::gfx::GfxInfoFeatureStatus>&& aFS);

  static bool OnlyAllowFeatureOnKnownConfig(int32_t aFeature);

  static bool MatchingRefreshRateStatus(RefreshRateStatus aSytemStatus,
                                        RefreshRateStatus aBlockedStatus);
  static bool MatchingRefreshRates(int32_t aSystem, int32_t aBlocked,
                                   int32_t aBlockedMax,
                                   VersionComparisonOp aCmp);

 protected:
  virtual ~GfxInfoBase();

  virtual OperatingSystem GetOperatingSystem() = 0;

  virtual nsresult GetFeatureStatusImpl(
      int32_t aFeature, int32_t* aStatus, nsAString& aSuggestedDriverVersion,
      const nsTArray<RefPtr<GfxDriverInfo>>& aDriverInfo,
      nsACString& aFailureId, OperatingSystem* aOS = nullptr);

  // Gets the driver info table. Used by GfxInfoBase to check for general cases
  // (while subclasses check for more specific ones).
  virtual const nsTArray<RefPtr<GfxDriverInfo>>& GetGfxDriverInfo() = 0;

  virtual void DescribeFeatures(JSContext* aCx, JS::Handle<JSObject*> obj);

  virtual bool DoesWindowProtocolMatch(
      const nsAString& aBlocklistWindowProtocol,
      const nsAString& aWindowProtocol);

  bool DoesVendorMatch(const nsAString& aBlocklistVendor,
                       const nsAString& aAdapterVendor);

  virtual bool DoesDriverVendorMatch(const nsAString& aBlocklistVendor,
                                     const nsAString& aDriverVendor);

  bool InitFeatureObject(JSContext* aCx, JS::Handle<JSObject*> aContainer,
                         const char* aName,
                         mozilla::gfx::FeatureState& aFeatureState,
                         JS::MutableHandle<JSObject*> aOutObj);

  NS_IMETHOD ControlGPUProcessForXPCShell(bool aEnable, bool* _retval) override;

  NS_IMETHOD KillGPUProcessForTests() override;
  NS_IMETHOD CrashGPUProcessForTests() override;

  // Total number of pixels for all detected screens at startup.
  int64_t mScreenPixels;
  size_t mScreenCount = 0;
  int32_t mMinRefreshRate = 0;
  int32_t mMaxRefreshRate = 0;

 private:
  virtual int32_t FindBlocklistedDeviceInList(
      const nsTArray<RefPtr<GfxDriverInfo>>& aDriverInfo,
      nsAString& aSuggestedVersion, int32_t aFeature, nsACString& aFailureId,
      OperatingSystem os, bool aForAllowing);

  std::pair<nsIGfxInfo::FontVisibilityDeviceDetermination, nsString>*
  GetFontVisibilityDeterminationPair();

  bool IsFeatureAllowlisted(int32_t aFeature) const;

  void EvaluateDownloadedBlocklist(
      nsTArray<RefPtr<GfxDriverInfo>>& aDriverInfo);

  bool BuildFeatureStateLog(JSContext* aCx, const gfx::FeatureState& aFeature,
                            JS::MutableHandle<JS::Value> aOut);

  Mutex mMutex MOZ_UNANNOTATED;
};

}  // namespace widget
}  // namespace mozilla

#endif /* __mozilla_widget_GfxInfoBase_h__ */
