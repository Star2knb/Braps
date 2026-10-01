// Vtable indices of the methods we hook (recorder plan §4.6.3), checked against the Windows SDK
// headers (dxgi.h, dxgi1_2.h, dxgi1_4.h, d3d9.h, d3d12.h): count the methods in declaration order,
// IUnknown's three first.
#pragma once

namespace rec::vt {

// IDXGISwapChain : IDXGIDeviceSubObject : IDXGIObject : IUnknown
//   IUnknown 0-2, IDXGIObject 3-6 (SetPrivateData, SetPrivateDataInterface, GetPrivateData, GetParent),
//   IDXGIDeviceSubObject 7 (GetDevice), IDXGISwapChain 8-17:
constexpr int kSwapChainPresent = 8;         // Present
constexpr int kSwapChainResizeBuffers = 13;  // GetBuffer 9, SetFullscreenState 10, GetFullscreenState 11, GetDesc 12
// IDXGISwapChain1 18-28: GetDesc1 18, GetFullscreenDesc 19, GetHwnd 20, GetCoreWindow 21, Present1 22, ...
constexpr int kSwapChain1Present1 = 22;
// IDXGISwapChain2 29-35, IDXGISwapChain3 36-39: GetCurrentBackBufferIndex 36, CheckColorSpaceSupport 37,
// SetColorSpace1 38, ResizeBuffers1 39
constexpr int kSwapChain3ResizeBuffers1 = 39;

// IDirect3DDevice9: TestCooperativeLevel 3 ... Reset 16, Present 17; Ex: PresentEx 121, ResetEx 132.
constexpr int kDevice9Reset = 16;
constexpr int kDevice9Present = 17;
constexpr int kDevice9ExPresentEx = 121;
constexpr int kDevice9ExResetEx = 132;
// IDirect3DSwapChain9: Present 3.
constexpr int kSwapChain9Present = 3;
// ID3D12CommandQueue: ExecuteCommandLists 10.
constexpr int kCommandQueueExecuteCommandLists = 10;

}  // namespace rec::vt
