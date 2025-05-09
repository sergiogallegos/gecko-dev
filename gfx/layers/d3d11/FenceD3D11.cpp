/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FenceD3D11.h"

#include <d3d11.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include "mozilla/gfx/Logging.h"

namespace mozilla {
namespace layers {

MOZ_RUNINIT RefPtr<ID3D11Device> mDevice;

/* static */
RefPtr<FenceD3D11> FenceD3D11::Create(ID3D11Device* aDevice) {
  MOZ_ASSERT(aDevice);

  if (!aDevice) {
    return nullptr;
  }

  RefPtr<ID3D11Device5> d3d11_5;
  auto hr =
      aDevice->QueryInterface(__uuidof(ID3D11Device5), getter_AddRefs(d3d11_5));
  if (FAILED(hr)) {
    gfxCriticalNoteOnce << "Failed to get ID3D11Device5: " << gfx::hexa(hr);
    return nullptr;
  }

  RefPtr<ID3D11Fence> fenceD3D11;
  d3d11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                       IID_PPV_ARGS((ID3D11Fence**)getter_AddRefs(fenceD3D11)));
  if (FAILED(hr) || !fenceD3D11) {
    gfxCriticalNoteOnce << "Fence creation failed: " << gfx::hexa(hr);
    return nullptr;
  }

  HANDLE sharedHandle = nullptr;
  hr = fenceD3D11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                      &sharedHandle);
  if (FAILED(hr)) {
    gfxCriticalNoteOnce << "Fence shared handle creation failed "
                        << gfx::hexa(hr);
    return nullptr;
  }

  RefPtr<gfx::FileHandleWrapper> handle =
      new gfx::FileHandleWrapper(UniqueFileHandle(sharedHandle));
  RefPtr<FenceD3D11> fence =
      new FenceD3D11(OwnsFence::Yes, aDevice, fenceD3D11, handle);
  return fence;
}

/* static */
RefPtr<FenceD3D11> FenceD3D11::CreateFromHandle(
    RefPtr<gfx::FileHandleWrapper> aHandle,
    const RefPtr<ID3D11Device> aDevice) {
  MOZ_ASSERT(aHandle);

  if (!aHandle) {
    return nullptr;
  }
  // Opening shared handle is deferred.
  return new FenceD3D11(OwnsFence::No, aDevice, /* aSignalFence */ nullptr,
                        aHandle);
}

/* static */
bool FenceD3D11::IsSupported(ID3D11Device* aDevice) {
  MOZ_ASSERT(aDevice);

  if (!aDevice) {
    return false;
  }
  RefPtr<ID3D11Device5> d3d11_5;
  auto hr = aDevice->QueryInterface((ID3D11Device5**)getter_AddRefs(d3d11_5));
  if (FAILED(hr)) {
    return false;
  }

  // Check for IDXGIAdapter4:
  RefPtr<IDXGIDevice> dxgiDevice;
  aDevice->QueryInterface((IDXGIDevice**)getter_AddRefs(dxgiDevice));
  if (FAILED(hr)) {
    return false;
  }

  RefPtr<IDXGIAdapter> dxgiAdapter;
  hr = dxgiDevice->GetAdapter(getter_AddRefs(dxgiAdapter));
  if (FAILED(hr)) {
    return false;
  }

  RefPtr<IDXGIAdapter4> dxgiAdapter4;
  dxgiAdapter->QueryInterface((IDXGIAdapter4**)getter_AddRefs(dxgiAdapter4));
  if (FAILED(hr)) {
    gfxCriticalNoteOnce << "Failed to get IDXGIAdapter4: " << gfx::hexa(hr);
    return false;
  }

  DXGI_ADAPTER_DESC3 adapterDesc;
  hr = dxgiAdapter4->GetDesc3(&adapterDesc);
  if (FAILED(hr)) {
    return false;
  }

  // The adapter must support monitored fences.
  return adapterDesc.Flags & DXGI_ADAPTER_FLAG3_SUPPORT_MONITORED_FENCES;
}

FenceD3D11::FenceD3D11(const OwnsFence aOwnsFence,
                       const RefPtr<ID3D11Device> aDevice,
                       const RefPtr<ID3D11Fence> aSignalFence,
                       const RefPtr<gfx::FileHandleWrapper>& aHandle)
    : mOwnsFence(aOwnsFence),
      mDevice(aDevice),
      mSignalFence(aSignalFence),
      mHandle(aHandle) {
  MOZ_ASSERT(mHandle);
  MOZ_ASSERT_IF(mOwnsFence == OwnsFence::Yes, mDevice);
  MOZ_ASSERT_IF(mOwnsFence == OwnsFence::Yes, mSignalFence);
  MOZ_ASSERT_IF(mOwnsFence == OwnsFence::No, !mSignalFence);
}

FenceD3D11::~FenceD3D11() {}

RefPtr<FenceD3D11> FenceD3D11::CloneFromHandle() {
  RefPtr<FenceD3D11> fence = FenceD3D11::CreateFromHandle(mHandle, mDevice);
  if (fence) {
    fence->Update(mFenceValue);
  }
  return fence;
}

bool FenceD3D11::IncrementAndSignal() {
  MOZ_ASSERT(mOwnsFence == OwnsFence::Yes);

  if (mOwnsFence != OwnsFence::Yes) {
    return false;
  }

  RefPtr<ID3D11DeviceContext> context;
  mDevice->GetImmediateContext(getter_AddRefs(context));
  RefPtr<ID3D11DeviceContext4> context4;
  auto hr = context->QueryInterface(__uuidof(ID3D11DeviceContext4),
                                    getter_AddRefs(context4));
  if (FAILED(hr)) {
    gfxCriticalNoteOnce << "Failed to get D3D11DeviceContext4: "
                        << gfx::hexa(hr);
    return false;
  }

  hr = context4->Signal(mSignalFence, mFenceValue + 1);
  if (FAILED(hr)) {
    gfxCriticalNoteOnce << "Signal fence failed: " << gfx::hexa(hr);
    return false;
  }

  mFenceValue++;
  return true;
}

void FenceD3D11::Update(uint64_t aFenceValue) {
  MOZ_ASSERT(mOwnsFence == OwnsFence::No);

  if (mOwnsFence != OwnsFence::No) {
    return;
  }

  if (mFenceValue > aFenceValue) {
    MOZ_ASSERT_UNREACHABLE("unexpected to be called");
    return;
  }
  mFenceValue = aFenceValue;
}

bool FenceD3D11::Wait(ID3D11Device* aDevice) {
  MOZ_ASSERT(aDevice);

  if (!aDevice) {
    return false;
  }

  // Skip wait if passed device is the same as signaling device.
  if (mDevice == aDevice) {
    return true;
  }

  RefPtr<ID3D11Fence> fence;
  auto it = mWaitFenceMap.find(aDevice);
  if (it == mWaitFenceMap.end()) {
    RefPtr<ID3D11Device5> d3d11_5;
    auto hr = aDevice->QueryInterface(__uuidof(ID3D11Device5),
                                      getter_AddRefs(d3d11_5));
    if (FAILED(hr)) {
      gfxCriticalNoteOnce << "Failed to get ID3D11Device5: " << gfx::hexa(hr);
      return false;
    }
    hr = d3d11_5->OpenSharedFence(mHandle->GetHandle(), __uuidof(ID3D11Fence),
                                  (void**)(ID3D11Fence**)getter_AddRefs(fence));
    if (FAILED(hr)) {
      gfxCriticalNoteOnce << "Opening fence shared handle failed "
                          << gfx::hexa(hr);
      return false;
    }
    mWaitFenceMap.emplace(aDevice, fence);
  } else {
    fence = it->second;
  }

  if (!fence) {
    MOZ_ASSERT_UNREACHABLE("unexpected to be called");
    return false;
  }

  RefPtr<ID3D11DeviceContext> context;
  aDevice->GetImmediateContext(getter_AddRefs(context));
  RefPtr<ID3D11DeviceContext4> context4;
  auto hr = context->QueryInterface(__uuidof(ID3D11DeviceContext4),
                                    getter_AddRefs(context4));
  if (FAILED(hr)) {
    gfxCriticalNoteOnce << "Failed to get D3D11DeviceContext4: "
                        << gfx::hexa(hr);
    return false;
  }
  hr = context4->Wait(fence, mFenceValue);
  if (FAILED(hr)) {
    gfxCriticalNoteOnce << "Failed to wait fence: " << gfx::hexa(hr);
    return false;
  }

  return true;
}

}  // namespace layers
}  // namespace mozilla
