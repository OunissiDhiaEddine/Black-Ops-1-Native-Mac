// Vtable slot lists for the D3D9 interfaces the proxy hooks. Single source of truth for
// thunks.S (slot -> thunk) and proxy.cpp (slot -> name). proxy.cpp static_asserts every
// entry against the Vtbl structs in the SDK header, so a wrong index cannot compile.
#pragma once

#define IDIRECT3D9_METHODS(X) \
    X(0, QueryInterface) X(1, AddRef) X(2, Release) X(3, RegisterSoftwareDevice) \
    X(4, GetAdapterCount) X(5, GetAdapterIdentifier) X(6, GetAdapterModeCount) \
    X(7, EnumAdapterModes) X(8, GetAdapterDisplayMode) X(9, CheckDeviceType) \
    X(10, CheckDeviceFormat) X(11, CheckDeviceMultiSampleType) X(12, CheckDepthStencilMatch) \
    X(13, CheckDeviceFormatConversion) X(14, GetDeviceCaps) X(15, GetAdapterMonitor) \
    X(16, CreateDevice)
#define IDIRECT3D9_COUNT 17

#define IDIRECT3DDEVICE9_METHODS(X) \
    X(0, QueryInterface) X(1, AddRef) X(2, Release) X(3, TestCooperativeLevel) \
    X(4, GetAvailableTextureMem) X(5, EvictManagedResources) X(6, GetDirect3D) \
    X(7, GetDeviceCaps) X(8, GetDisplayMode) X(9, GetCreationParameters) \
    X(10, SetCursorProperties) X(11, SetCursorPosition) X(12, ShowCursor) \
    X(13, CreateAdditionalSwapChain) X(14, GetSwapChain) X(15, GetNumberOfSwapChains) \
    X(16, Reset) X(17, Present) X(18, GetBackBuffer) X(19, GetRasterStatus) \
    X(20, SetDialogBoxMode) X(21, SetGammaRamp) X(22, GetGammaRamp) X(23, CreateTexture) \
    X(24, CreateVolumeTexture) X(25, CreateCubeTexture) X(26, CreateVertexBuffer) \
    X(27, CreateIndexBuffer) X(28, CreateRenderTarget) X(29, CreateDepthStencilSurface) \
    X(30, UpdateSurface) X(31, UpdateTexture) X(32, GetRenderTargetData) \
    X(33, GetFrontBufferData) X(34, StretchRect) X(35, ColorFill) \
    X(36, CreateOffscreenPlainSurface) X(37, SetRenderTarget) X(38, GetRenderTarget) \
    X(39, SetDepthStencilSurface) X(40, GetDepthStencilSurface) X(41, BeginScene) \
    X(42, EndScene) X(43, Clear) X(44, SetTransform) X(45, GetTransform) \
    X(46, MultiplyTransform) X(47, SetViewport) X(48, GetViewport) X(49, SetMaterial) \
    X(50, GetMaterial) X(51, SetLight) X(52, GetLight) X(53, LightEnable) \
    X(54, GetLightEnable) X(55, SetClipPlane) X(56, GetClipPlane) X(57, SetRenderState) \
    X(58, GetRenderState) X(59, CreateStateBlock) X(60, BeginStateBlock) X(61, EndStateBlock) \
    X(62, SetClipStatus) X(63, GetClipStatus) X(64, GetTexture) X(65, SetTexture) \
    X(66, GetTextureStageState) X(67, SetTextureStageState) X(68, GetSamplerState) \
    X(69, SetSamplerState) X(70, ValidateDevice) X(71, SetPaletteEntries) \
    X(72, GetPaletteEntries) X(73, SetCurrentTexturePalette) X(74, GetCurrentTexturePalette) \
    X(75, SetScissorRect) X(76, GetScissorRect) X(77, SetSoftwareVertexProcessing) \
    X(78, GetSoftwareVertexProcessing) X(79, SetNPatchMode) X(80, GetNPatchMode) \
    X(81, DrawPrimitive) X(82, DrawIndexedPrimitive) X(83, DrawPrimitiveUP) \
    X(84, DrawIndexedPrimitiveUP) X(85, ProcessVertices) X(86, CreateVertexDeclaration) \
    X(87, SetVertexDeclaration) X(88, GetVertexDeclaration) X(89, SetFVF) X(90, GetFVF) \
    X(91, CreateVertexShader) X(92, SetVertexShader) X(93, GetVertexShader) \
    X(94, SetVertexShaderConstantF) X(95, GetVertexShaderConstantF) \
    X(96, SetVertexShaderConstantI) X(97, GetVertexShaderConstantI) \
    X(98, SetVertexShaderConstantB) X(99, GetVertexShaderConstantB) X(100, SetStreamSource) \
    X(101, GetStreamSource) X(102, SetStreamSourceFreq) X(103, GetStreamSourceFreq) \
    X(104, SetIndices) X(105, GetIndices) X(106, CreatePixelShader) X(107, SetPixelShader) \
    X(108, GetPixelShader) X(109, SetPixelShaderConstantF) X(110, GetPixelShaderConstantF) \
    X(111, SetPixelShaderConstantI) X(112, GetPixelShaderConstantI) \
    X(113, SetPixelShaderConstantB) X(114, GetPixelShaderConstantB) X(115, DrawRectPatch) \
    X(116, DrawTriPatch) X(117, DeletePatch) X(118, CreateQuery)
#define IDIRECT3DDEVICE9_COUNT 119

#define IDIRECT3DSWAPCHAIN9_METHODS(X) \
    X(0, QueryInterface) X(1, AddRef) X(2, Release) X(3, Present) X(4, GetFrontBufferData) \
    X(5, GetBackBuffer) X(6, GetRasterStatus) X(7, GetDisplayMode) X(8, GetDevice) \
    X(9, GetPresentParameters)
#define IDIRECT3DSWAPCHAIN9_COUNT 10

// Interface ids used in the log and by the thunks.
#define IFACE_D3D9 0
#define IFACE_DEVICE 1
#define IFACE_SWAPCHAIN 2
#define IFACE_COUNT 3
