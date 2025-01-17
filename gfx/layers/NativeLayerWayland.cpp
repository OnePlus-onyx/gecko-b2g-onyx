/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/NativeLayerWayland.h"

#include <utility>
#include <algorithm>

#include "gfxUtils.h"
#include "GLContextProvider.h"
#include "mozilla/layers/SurfacePoolWayland.h"
#include "mozilla/StaticPrefs_widget.h"
#include "mozilla/webrender/RenderThread.h"

namespace mozilla::layers {

using gfx::Point;
using gfx::Rect;
using gfx::Size;

/* static */
already_AddRefed<NativeLayerRootWayland>
NativeLayerRootWayland::CreateForMozContainer(MozContainer* aContainer) {
  RefPtr<NativeLayerRootWayland> layerRoot =
      new NativeLayerRootWayland(aContainer);
  return layerRoot.forget();
}

NativeLayerRootWayland::NativeLayerRootWayland(MozContainer* aContainer)
    : mMutex("NativeLayerRootWayland"), mContainer(aContainer) {}

void NativeLayerRootWayland::EnsureSurfaceInitialized() {
  if (mShmBuffer) {
    return;
  }

  wl_surface* wlSurface = moz_container_wayland_surface_lock(mContainer);
  if (wlSurface) {
    mShmBuffer = widget::WaylandShmBuffer::Create(widget::WaylandDisplayGet(),
                                                  LayoutDeviceIntSize(1, 1));
    mShmBuffer->Clear();

    mShmBuffer->AttachAndCommit(wlSurface);
    moz_container_wayland_surface_unlock(mContainer, &wlSurface);
  }
}

already_AddRefed<NativeLayer> NativeLayerRootWayland::CreateLayer(
    const IntSize& aSize, bool aIsOpaque,
    SurfacePoolHandle* aSurfacePoolHandle) {
  RefPtr<NativeLayer> layer = new NativeLayerWayland(
      aSize, aIsOpaque, aSurfacePoolHandle->AsSurfacePoolHandleWayland());
  return layer.forget();
}

already_AddRefed<NativeLayer>
NativeLayerRootWayland::CreateLayerForExternalTexture(bool aIsOpaque) {
  RefPtr<NativeLayer> layer = new NativeLayerWayland(aIsOpaque);
  return layer.forget();
}

void NativeLayerRootWayland::AppendLayer(NativeLayer* aLayer) {
  MOZ_RELEASE_ASSERT(false);
  MutexAutoLock lock(mMutex);

  RefPtr<NativeLayerWayland> layerWayland = aLayer->AsNativeLayerWayland();
  MOZ_RELEASE_ASSERT(layerWayland);

  mSublayers.AppendElement(layerWayland);
  layerWayland->SetBackingScale(mBackingScale);
}

void NativeLayerRootWayland::RemoveLayer(NativeLayer* aLayer) {
  MOZ_RELEASE_ASSERT(false);
  MutexAutoLock lock(mMutex);

  RefPtr<NativeLayerWayland> layerWayland = aLayer->AsNativeLayerWayland();
  MOZ_RELEASE_ASSERT(layerWayland);

  mSublayers.RemoveElement(layerWayland);
}

bool NativeLayerRootWayland::EnsureShowLayer(
    const RefPtr<NativeLayerWayland>& aLayer) {
  if (aLayer->mIsShown) {
    return true;
  }

  RefPtr<NativeSurfaceWayland> nativeSurface = aLayer->mNativeSurface;
  if (!nativeSurface->mWlSubsurface) {
    wl_surface* wlSurface = moz_container_wayland_surface_lock(mContainer);
    if (!wlSurface) {
      return false;
    }

    wl_subcompositor* subcompositor =
        widget::WaylandDisplayGet()->GetSubcompositor();

    nativeSurface->mWlSubsurface = wl_subcompositor_get_subsurface(
        subcompositor, nativeSurface->mWlSurface, wlSurface);

    moz_container_wayland_surface_unlock(mContainer, &wlSurface);
  }

  aLayer->mIsShown = true;
  return true;
}

void NativeLayerRootWayland::EnsureHideLayer(
    const RefPtr<NativeLayerWayland>& aLayer) {
  if (!aLayer->mIsShown) {
    return;
  }

  RefPtr<NativeSurfaceWayland> nativeSurface = aLayer->mNativeSurface;

  wl_subsurface_set_position(nativeSurface->mWlSubsurface, 20, 20);
  wp_viewport_set_source(nativeSurface->mViewport, wl_fixed_from_int(0),
                         wl_fixed_from_int(0), wl_fixed_from_int(1),
                         wl_fixed_from_int(1));
  wp_viewport_set_destination(nativeSurface->mViewport, 1, 1);
  wl_surface_commit(nativeSurface->mWlSurface);

  wl_surface* wlSurface = moz_container_wayland_surface_lock(mContainer);
  if (wlSurface) {
    wl_subsurface_place_below(nativeSurface->mWlSubsurface, wlSurface);
    moz_container_wayland_surface_unlock(mContainer, &wlSurface);
  }

  aLayer->mIsShown = false;
}

void NativeLayerRootWayland::UnmapLayer(
    const RefPtr<NativeLayerWayland>& aLayer) {
  RefPtr<NativeSurfaceWayland> nativeSurface = aLayer->mNativeSurface;
  g_clear_pointer(&nativeSurface->mWlSubsurface, wl_subsurface_destroy);
  aLayer->mIsShown = false;
}

void NativeLayerRootWayland::SetLayers(
    const nsTArray<RefPtr<NativeLayer>>& aLayers) {
  MutexAutoLock lock(mMutex);

  // Ideally, we'd just be able to do mSublayers = std::move(aLayers).
  // However, aLayers has a different type: it carries NativeLayer objects,
  // whereas mSublayers carries NativeLayerWayland objects, so we have to
  // downcast all the elements first. There's one other reason to look at all
  // the elements in aLayers first: We need to make sure any new layers know
  // about our current backing scale.

  nsTArray<RefPtr<NativeLayerWayland>> newSublayers(aLayers.Length());
  for (const RefPtr<NativeLayer>& layer : aLayers) {
    RefPtr<NativeLayerWayland> layerWayland = layer->AsNativeLayerWayland();
    MOZ_RELEASE_ASSERT(layerWayland);
    layerWayland->SetBackingScale(mBackingScale);
    newSublayers.AppendElement(std::move(layerWayland));
  }

  if (newSublayers != mSublayers) {
    for (const RefPtr<NativeLayerWayland>& layer : mSublayers) {
      if (!newSublayers.Contains(layer)) {
        EnsureHideLayer(layer);
      }
    }
    mSublayers = std::move(newSublayers);
  }

  wl_surface* previousSurface = nullptr;
  for (const RefPtr<NativeLayerWayland>& layer : mSublayers) {
    RefPtr<NativeSurfaceWayland> nativeSurface = layer->mNativeSurface;

    Rect surfaceRectClipped =
        Rect(0, 0, (float)layer->mSize.width, (float)layer->mSize.height);
    surfaceRectClipped = surfaceRectClipped.Intersect(Rect(layer->mValidRect));

    Point relPosition = layer->mTransform.TransformPoint(Point(0, 0));
    Point absPosition = Point((float)layer->mPosition.x + relPosition.x,
                              (float)layer->mPosition.y + relPosition.y);

    Point scaledSize =
        layer->mTransform.TransformPoint(
            Point((float)layer->mSize.width, (float)layer->mSize.height)) -
        relPosition;
    float scaleX = scaledSize.x / (float)layer->mSize.width;
    float scaleY = scaledSize.y / (float)layer->mSize.height;

    surfaceRectClipped.x += absPosition.x;
    surfaceRectClipped.y += absPosition.y;
    surfaceRectClipped.width *= scaleX;
    surfaceRectClipped.height *= scaleY;

    if (layer->mClipRect) {
      surfaceRectClipped =
          surfaceRectClipped.Intersect(Rect(layer->mClipRect.value()));
    }

    if (roundf(surfaceRectClipped.width) > 0 &&
        roundf(surfaceRectClipped.height) > 0) {
      if (!EnsureShowLayer(layer)) {
        continue;
      }
    } else {
      EnsureHideLayer(layer);
      continue;
    }

    double scale = moz_container_wayland_get_scale(mContainer);
    wl_subsurface_set_position(nativeSurface->mWlSubsurface,
                               floor(surfaceRectClipped.x / scale),
                               floor(surfaceRectClipped.y / scale));
    wp_viewport_set_destination(nativeSurface->mViewport,
                                ceil(surfaceRectClipped.width / scale),
                                ceil(surfaceRectClipped.height / scale));

    Rect bufferClip = Rect(surfaceRectClipped.x - absPosition.x,
                           surfaceRectClipped.y - absPosition.y,
                           surfaceRectClipped.width, surfaceRectClipped.height);

    bufferClip.x /= scaleX;
    bufferClip.y /= scaleY;
    bufferClip.width /= scaleX;
    bufferClip.height /= scaleY;

    wp_viewport_set_source(nativeSurface->mViewport,
                           wl_fixed_from_double(bufferClip.x),
                           wl_fixed_from_double(bufferClip.y),
                           wl_fixed_from_double(bufferClip.width),
                           wl_fixed_from_double(bufferClip.height));

    wl_compositor* compositor = widget::WaylandDisplayGet()->GetCompositor();
    struct wl_region* region = wl_compositor_create_region(compositor);
    if (layer->mIsOpaque &&
        StaticPrefs::widget_wayland_opaque_region_enabled_AtStartup()) {
      wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
    }
    wl_surface_set_opaque_region(nativeSurface->mWlSurface, region);
    wl_region_destroy(region);

    if (previousSurface) {
      wl_subsurface_place_above(nativeSurface->mWlSubsurface, previousSurface);
      previousSurface = nativeSurface->mWlSurface;
    } else {
      wl_surface* wlSurface = moz_container_wayland_surface_lock(mContainer);
      if (wlSurface) {
        wl_subsurface_place_above(nativeSurface->mWlSubsurface, wlSurface);
        moz_container_wayland_surface_unlock(mContainer, &wlSurface);
      }
      previousSurface = nativeSurface->mWlSurface;
    }
  }
}

void NativeLayerRootWayland::SetBackingScale(float aBackingScale) {
  MutexAutoLock lock(mMutex);

  mBackingScale = aBackingScale;
  for (const RefPtr<NativeLayerWayland>& layer : mSublayers) {
    layer->SetBackingScale(aBackingScale);
  }
}

float NativeLayerRootWayland::BackingScale() {
  MutexAutoLock lock(mMutex);
  return mBackingScale;
}

bool NativeLayerRootWayland::CommitToScreen() {
  MutexAutoLock lock(mMutex);

  wl_surface* wlSurface = moz_container_wayland_surface_lock(mContainer);
  for (RefPtr<NativeLayerWayland>& layer : mSublayers) {
    layer->mNativeSurface->Commit(layer->mDirtyRegion);
    layer->mDirtyRegion.SetEmpty();
  }

  if (wlSurface) {
    wl_surface_commit(wlSurface);
    moz_container_wayland_surface_unlock(mContainer, &wlSurface);
  }

  EnsureSurfaceInitialized();
  return true;
}

void NativeLayerRootWayland::PauseCompositor() {
  MutexAutoLock lock(mMutex);

  for (RefPtr<NativeLayerWayland>& layer : mSublayers) {
    UnmapLayer(layer);
  }

  mShmBuffer = nullptr;
}

bool NativeLayerRootWayland::ResumeCompositor() { return true; }

UniquePtr<NativeLayerRootSnapshotter>
NativeLayerRootWayland::CreateSnapshotter() {
  MutexAutoLock lock(mMutex);
  return nullptr;
}

NativeLayerWayland::NativeLayerWayland(
    const IntSize& aSize, bool aIsOpaque,
    SurfacePoolHandleWayland* aSurfacePoolHandle)
    : mMutex("NativeLayerWayland"),
      mSurfacePoolHandle(aSurfacePoolHandle),
      mSize(aSize),
      mIsOpaque(aIsOpaque),
      mNativeSurface(nullptr) {
  MOZ_RELEASE_ASSERT(mSurfacePoolHandle,
                     "Need a non-null surface pool handle.");
}

NativeLayerWayland::NativeLayerWayland(bool aIsOpaque)
    : mMutex("NativeLayerWayland"),
      mSurfacePoolHandle(nullptr),
      mIsOpaque(aIsOpaque) {
  MOZ_RELEASE_ASSERT(false);  // external image
}

NativeLayerWayland::~NativeLayerWayland() {
  if (mNativeSurface) {
    mSurfacePoolHandle->ReturnSurfaceToPool(mNativeSurface);
  }
}

void NativeLayerWayland::AttachExternalImage(
    wr::RenderTextureHost* aExternalImage) {
  MOZ_RELEASE_ASSERT(false);
}

void NativeLayerWayland::SetSurfaceIsFlipped(bool aIsFlipped) {
  MutexAutoLock lock(mMutex);

  if (aIsFlipped != mSurfaceIsFlipped) {
    mSurfaceIsFlipped = aIsFlipped;
  }
}

bool NativeLayerWayland::SurfaceIsFlipped() {
  MutexAutoLock lock(mMutex);

  return mSurfaceIsFlipped;
}

IntSize NativeLayerWayland::GetSize() {
  MutexAutoLock lock(mMutex);
  return mSize;
}

void NativeLayerWayland::SetPosition(const IntPoint& aPosition) {
  MutexAutoLock lock(mMutex);

  if (aPosition != mPosition) {
    mPosition = aPosition;
  }
}

IntPoint NativeLayerWayland::GetPosition() {
  MutexAutoLock lock(mMutex);
  return mPosition;
}

void NativeLayerWayland::SetTransform(const Matrix4x4& aTransform) {
  MutexAutoLock lock(mMutex);
  MOZ_ASSERT(aTransform.IsRectilinear());

  if (aTransform != mTransform) {
    mTransform = aTransform;
  }
}

void NativeLayerWayland::SetSamplingFilter(SamplingFilter aSamplingFilter) {
  MutexAutoLock lock(mMutex);

  if (aSamplingFilter != mSamplingFilter) {
    mSamplingFilter = aSamplingFilter;
  }
}

Matrix4x4 NativeLayerWayland::GetTransform() {
  MutexAutoLock lock(mMutex);
  return mTransform;
}

IntRect NativeLayerWayland::GetRect() {
  MutexAutoLock lock(mMutex);
  return IntRect(mPosition, mSize);
}

void NativeLayerWayland::SetBackingScale(float aBackingScale) {
  MutexAutoLock lock(mMutex);

  if (aBackingScale != mBackingScale) {
    mBackingScale = aBackingScale;
  }
}

bool NativeLayerWayland::IsOpaque() {
  MutexAutoLock lock(mMutex);
  return mIsOpaque;
}

void NativeLayerWayland::SetClipRect(const Maybe<IntRect>& aClipRect) {
  MutexAutoLock lock(mMutex);

  if (aClipRect != mClipRect) {
    mClipRect = aClipRect;
  }
}

Maybe<IntRect> NativeLayerWayland::ClipRect() {
  MutexAutoLock lock(mMutex);
  return mClipRect;
}

IntRect NativeLayerWayland::CurrentSurfaceDisplayRect() {
  MutexAutoLock lock(mMutex);
  return mDisplayRect;
}

RefPtr<DrawTarget> NativeLayerWayland::NextSurfaceAsDrawTarget(
    const IntRect& aDisplayRect, const IntRegion& aUpdateRegion,
    BackendType aBackendType) {
  MutexAutoLock lock(mMutex);

  mValidRect = IntRect(aDisplayRect);
  mDirtyRegion = IntRegion(aUpdateRegion);

  if (!mNativeSurface) {
    mNativeSurface = mSurfacePoolHandle->ObtainSurfaceFromPool(mSize);
  }

  return mNativeSurface->GetAsDrawTarget();
}

Maybe<GLuint> NativeLayerWayland::NextSurfaceAsFramebuffer(
    const IntRect& aDisplayRect, const IntRegion& aUpdateRegion,
    bool aNeedsDepth) {
  MutexAutoLock lock(mMutex);

  mValidRect = IntRect(aDisplayRect);
  mDirtyRegion = IntRegion(aUpdateRegion);

  if (!mNativeSurface) {
    mNativeSurface = mSurfacePoolHandle->ObtainSurfaceFromPool(mSize);
  }

  return mNativeSurface ? mNativeSurface->GetAsFramebuffer() : Nothing();
}

void NativeLayerWayland::NotifySurfaceReady() {
  MutexAutoLock lock(mMutex);

  mNativeSurface->NotifySurfaceReady();
}

void NativeLayerWayland::DiscardBackbuffers() {}

}  // namespace mozilla::layers
