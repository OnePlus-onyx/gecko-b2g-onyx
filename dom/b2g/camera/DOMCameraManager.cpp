/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DOMCameraManager.h"
#include "nsDebug.h"
#include "jsapi.h"
#include "B2G.h"
#include "nsPIDOMWindow.h"
#include "mozilla/Services.h"
#include "nsContentPermissionHelper.h"
#include "nsIContentPermissionPrompt.h"
#include "nsIObserverService.h"
#include "nsIPermissionManager.h"
#include "nsIScriptObjectPrincipal.h"
#include "DOMCameraControl.h"
//#include "nsDOMClassInfo.h" //TODO
#include "CameraCommon.h"
#include "CameraPreferences.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/PermissionMessageUtils.h"
#include "nsQueryObject.h"

using namespace mozilla;
using namespace mozilla::dom;

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(nsDOMCameraManager, mWindow)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(nsDOMCameraManager)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIObserver)
  NS_INTERFACE_MAP_ENTRY(nsISupportsWeakReference)
  NS_INTERFACE_MAP_ENTRY(nsIObserver)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(nsDOMCameraManager)
NS_IMPL_CYCLE_COLLECTING_RELEASE(nsDOMCameraManager)

/**
 * Global camera logging object
 *
 * Set the MOZ_LOG environment variable to enable logging
 * in a debug build, e.g. MOZ_LOG=Camera:5
 */
LogModule* GetCameraLog() {
  static LazyLogModule sLog("Camera");
  return sLog;
}

::WindowTable* nsDOMCameraManager::sActiveWindows = nullptr;

nsDOMCameraManager::nsDOMCameraManager(nsPIDOMWindowInner* aWindow)
    : mWindowId(aWindow->WindowID()),
      mPermission(nsIPermissionManager::DENY_ACTION),
      mWindow(aWindow) {
  /* member initializers and constructor code */
  DOM_CAMERA_LOGT("%s:%d : this=%p, windowId=%" PRIx64 "\n", __func__, __LINE__,
                  this, mWindowId);
}

nsDOMCameraManager::~nsDOMCameraManager() {
  /* destructor code */
  DOM_CAMERA_LOGT("%s:%d : this=%p\n", __func__, __LINE__, this);
}

/* static */
void nsDOMCameraManager::GetListOfCameras(nsTArray<nsString>& aList,
                                          ErrorResult& aRv) {
  aRv = ICameraControl::GetListOfCameras(aList);
}

/* static */
bool nsDOMCameraManager::HasSupport(JSContext* aCx, JSObject* aGlobal) {
  return B2G::HasCameraSupport(aCx, aGlobal);
}

/* static */
already_AddRefed<nsDOMCameraManager> nsDOMCameraManager::CreateInstance(
    nsPIDOMWindowInner* aWindow) {
  // Initialize the shared active window tracker
  if (!sActiveWindows) {
    sActiveWindows = new ::WindowTable();
  }

  RefPtr<nsDOMCameraManager> cameraManager = new nsDOMCameraManager(aWindow);

  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (!obs) {
    DOM_CAMERA_LOGE("Camera manager failed to get observer service\n");
    return nullptr;
  }

  nsresult rv = obs->AddObserver(cameraManager, "xpcom-shutdown", true);
  if (NS_FAILED(rv)) {
    DOM_CAMERA_LOGE(
        "Camera manager failed to add 'xpcom-shutdown' observer (0x%x)\n", rv);
    return nullptr;
  }

  return cameraManager.forget();
}

class CameraPermissionRequest : public nsIContentPermissionRequest,
                                public nsIRunnable {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_NSICONTENTPERMISSIONREQUEST
  NS_DECL_NSIRUNNABLE
  NS_DECL_CYCLE_COLLECTION_CLASS_AMBIGUOUS(CameraPermissionRequest,
                                           nsIContentPermissionRequest)

  CameraPermissionRequest(nsIPrincipal* aPrincipal, nsPIDOMWindowInner* aWindow,
                          RefPtr<nsDOMCameraManager> aManager,
                          uint32_t aCameraId,
                          const CameraConfiguration& aInitialConfig,
                          RefPtr<Promise> aPromise)
      : mPrincipal(aPrincipal),
        mWindow(aWindow),
        mCameraManager(aManager),
        mCameraId(aCameraId),
        mInitialConfig(aInitialConfig),
        mPromise(aPromise) {}

 protected:
  virtual ~CameraPermissionRequest() {}

  nsresult DispatchCallback(uint32_t aPermission);
  void CallAllow();
  void CallCancel();
  nsCOMPtr<nsIPrincipal> mPrincipal;
  nsCOMPtr<nsPIDOMWindowInner> mWindow;
  RefPtr<nsDOMCameraManager> mCameraManager;
  uint32_t mCameraId;
  CameraConfiguration mInitialConfig;
  RefPtr<Promise> mPromise;
};

NS_IMPL_CYCLE_COLLECTION(CameraPermissionRequest, mWindow, mPromise)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(CameraPermissionRequest)
  NS_INTERFACE_MAP_ENTRY(nsIContentPermissionRequest)
  NS_INTERFACE_MAP_ENTRY(nsIRunnable)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIContentPermissionRequest)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(CameraPermissionRequest)
NS_IMPL_CYCLE_COLLECTING_RELEASE(CameraPermissionRequest)

NS_IMETHODIMP
CameraPermissionRequest::Run() {
  return nsContentPermissionUtils::AskPermission(this, mWindow);
}

NS_IMETHODIMP
CameraPermissionRequest::GetPrincipal(nsIPrincipal** aRequestingPrincipal) {
  NS_ADDREF(*aRequestingPrincipal = mPrincipal);
  return NS_OK;
}

NS_IMETHODIMP
CameraPermissionRequest::GetWindow(mozIDOMWindow** aRequestingWindow) {
  NS_ADDREF(*aRequestingWindow = mWindow);
  return NS_OK;
}

NS_IMETHODIMP
CameraPermissionRequest::GetElement(mozilla::dom::Element** aElement) {
  *aElement = nullptr;
  return NS_OK;
}

NS_IMETHODIMP
CameraPermissionRequest::Cancel() {
  return DispatchCallback(nsIPermissionManager::DENY_ACTION);
}

NS_IMETHODIMP
CameraPermissionRequest::Allow(JS::HandleValue aChoices) {
  MOZ_ASSERT(aChoices.isUndefined());
  return DispatchCallback(nsIPermissionManager::ALLOW_ACTION);
}

nsresult CameraPermissionRequest::DispatchCallback(uint32_t aPermission) {
  nsCOMPtr<nsIRunnable> callbackRunnable;
  if (aPermission == nsIPermissionManager::ALLOW_ACTION) {
    callbackRunnable =
        NewRunnableMethod("CameraPermissionRequest::DispatchCallback", this,
                          &CameraPermissionRequest::CallAllow);
  } else {
    callbackRunnable =
        NewRunnableMethod("CameraPermissionRequest::DispatchCallback", this,
                          &CameraPermissionRequest::CallCancel);
  }
  return NS_DispatchToMainThread(callbackRunnable.forget());
}

void CameraPermissionRequest::CallAllow() {
  mCameraManager->PermissionAllowed(mCameraId, mInitialConfig, mPromise);
}

void CameraPermissionRequest::CallCancel() {
  mCameraManager->PermissionCancelled(mCameraId, mInitialConfig, mPromise);
}

NS_IMETHODIMP
CameraPermissionRequest::GetTypes(nsIArray** aTypes) {
  nsTArray<nsString> emptyOptions;
  return nsContentPermissionUtils::CreatePermissionArray("camera"_ns,
                                                         emptyOptions, aTypes);
}

NS_IMETHODIMP
CameraPermissionRequest::GetTopLevelPrincipal(
    nsIPrincipal** aTopLevelPrincipal) {
  return NS_OK;
}

NS_IMETHODIMP
CameraPermissionRequest::GetIsHandlingUserInput(bool* aHandlingUserInput) {
  return NS_OK;
}

NS_IMETHODIMP
CameraPermissionRequest::GetDelegatePrincipal(
    const nsACString& aType, nsIPrincipal** aRequestingPrincipal) {
  return PermissionDelegateHandler::GetDelegatePrincipal(aType, this,
                                                         aRequestingPrincipal);
}

NS_IMETHODIMP
CameraPermissionRequest::GetMaybeUnsafePermissionDelegate(
    bool* aMaybeUnsafePermissionDelegate) {
  return NS_OK;
}

#ifdef MOZ_WIDGET_GONK
/* static */ void nsDOMCameraManager::PreinitCameraHardware() {
  nsDOMCameraControl::PreinitCameraHardware();
}
#endif

already_AddRefed<Promise> nsDOMCameraManager::GetCamera(
    const nsAString& aCamera, const CameraConfiguration& aInitialConfig,
    ErrorResult& aRv) {
  DOM_CAMERA_LOGT("%s:%d\n", __func__, __LINE__);

  uint32_t cameraId = 0;  // back (or forward-facing) camera by default
  if (aCamera.EqualsLiteral("front")) {
    cameraId = 1;
  }

  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(mWindow);
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  RefPtr<Promise> promise = Promise::Create(global, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  if (mPermission == nsIPermissionManager::ALLOW_ACTION) {
    PermissionAllowed(cameraId, aInitialConfig, promise);
    return promise.forget();
  }

  nsCOMPtr<nsIScriptObjectPrincipal> sop = do_QueryInterface(mWindow);
  if (!sop) {
    aRv.Throw(NS_ERROR_UNEXPECTED);
    return nullptr;
  }

  nsCOMPtr<nsIPrincipal> principal = sop->GetPrincipal();
  // Unprivileged mochitests always fail the dispatched permission check,
  // even if permission to the camera has been granted.
  bool isMochitest = false;
  CameraPreferences::GetPref("camera.control.test.permission", isMochitest);
  if (isMochitest || B2G::CheckPermission("camera"_ns, mWindow)) {
    PermissionAllowed(cameraId, aInitialConfig, promise);
    return promise.forget();
  }

  nsCOMPtr<nsIRunnable> permissionRequest = new CameraPermissionRequest(
      principal, mWindow, this, cameraId, aInitialConfig, promise);
  NS_DispatchToMainThread(permissionRequest);
  return promise.forget();
}

void nsDOMCameraManager::PermissionAllowed(
    uint32_t aCameraId, const CameraConfiguration& aInitialConfig,
    Promise* aPromise) {
  mPermission = nsIPermissionManager::ALLOW_ACTION;

  // Creating this object will trigger the aOnSuccess callback
  //  (or the aOnError one, if it fails).
  RefPtr<nsDOMCameraControl> cameraControl =
      new nsDOMCameraControl(aCameraId, aInitialConfig, aPromise, mWindow);

  Register(cameraControl);
}

void nsDOMCameraManager::PermissionCancelled(
    uint32_t aCameraId, const CameraConfiguration& aInitialConfig,
    Promise* aPromise) {
  mPermission = nsIPermissionManager::DENY_ACTION;
  aPromise->MaybeReject(NS_ERROR_DOM_SECURITY_ERR);
}

void nsDOMCameraManager::Register(nsDOMCameraControl* aDOMCameraControl) {
  DOM_CAMERA_LOGI(
      ">>> Register( aDOMCameraControl = %p ) mWindowId = 0x%" PRIx64 "\n",
      aDOMCameraControl, mWindowId);
  MOZ_ASSERT(NS_IsMainThread());

  CameraControls* controls = sActiveWindows->Get(mWindowId);
  if (!controls) {
    controls = sActiveWindows->InsertOrUpdate(mWindowId, MakeUnique<CameraControls>()).get();
  }

  // Remove any stale CameraControl objects to limit our memory usage
  uint32_t i = controls->Length();
  while (i > 0) {
    --i;
    RefPtr<nsDOMCameraControl> cameraControl =
        do_QueryObject(controls->ElementAt(i));
    if (!cameraControl) {
      controls->RemoveElementAt(i);
    }
  }

  // Put the camera control into the hash table
  nsWeakPtr cameraControl =
      do_GetWeakReference(static_cast<DOMMediaStream*>(aDOMCameraControl));
  controls->AppendElement(cameraControl);
}

void nsDOMCameraManager::Shutdown(uint64_t aWindowId) {
  DOM_CAMERA_LOGI(">>> Shutdown( aWindowId = 0x%" PRIx64 " )\n", aWindowId);
  MOZ_ASSERT(NS_IsMainThread());

  CameraControls* controls = sActiveWindows->Get(aWindowId);
  if (!controls) {
    return;
  }

  uint32_t i = controls->Length();
  while (i > 0) {
    --i;
    RefPtr<nsDOMCameraControl> cameraControl =
        do_QueryObject(controls->ElementAt(i));
    if (cameraControl) {
      cameraControl->Shutdown();
    }
  }
  controls->Clear();

  sActiveWindows->Remove(aWindowId);
}

void nsDOMCameraManager::XpComShutdown() {
  DOM_CAMERA_LOGI(">>> XPCOM Shutdown\n");
  MOZ_ASSERT(NS_IsMainThread());

  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  obs->RemoveObserver(this, "xpcom-shutdown");

  delete sActiveWindows;
  sActiveWindows = nullptr;
}

nsresult nsDOMCameraManager::Observe(nsISupports* aSubject, const char* aTopic,
                                     const char16_t* aData) {
  if (strcmp(aTopic, "xpcom-shutdown") == 0) {
    XpComShutdown();
  }
  return NS_OK;
}

void nsDOMCameraManager::OnNavigation(uint64_t aWindowId) {
  DOM_CAMERA_LOGI(">>> OnNavigation event\n");
  Shutdown(aWindowId);
}

bool nsDOMCameraManager::IsWindowStillActive(uint64_t aWindowId) {
  MOZ_ASSERT(NS_IsMainThread());

  if (!sActiveWindows) {
    return false;
  }

  return !!sActiveWindows->Get(aWindowId);
}

JSObject* nsDOMCameraManager::WrapObject(JSContext* aCx,
                                         JS::Handle<JSObject*> aGivenProto) {
  return CameraManager_Binding::Wrap(aCx, this, aGivenProto);
}
