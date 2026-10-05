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

#define RESOURCE_METHODS(X) \
    X(0, QueryInterface) X(1, AddRef) X(2, Release) X(3, GetDevice) X(4, SetPrivateData) \
    X(5, GetPrivateData) X(6, FreePrivateData) X(7, SetPriority) X(8, GetPriority) X(9, PreLoad) \
    X(10, GetType)
#define BASETEXTURE_METHODS(X) \
    RESOURCE_METHODS(X) X(11, SetLOD) X(12, GetLOD) X(13, GetLevelCount) \
    X(14, SetAutoGenFilterType) X(15, GetAutoGenFilterType) X(16, GenerateMipSubLevels)

#define IDIRECT3DTEXTURE9_METHODS(X) \
    BASETEXTURE_METHODS(X) X(17, GetLevelDesc) X(18, GetSurfaceLevel) X(19, LockRect) \
    X(20, UnlockRect) X(21, AddDirtyRect)
#define IDIRECT3DTEXTURE9_COUNT 22

#define IDIRECT3DCUBETEXTURE9_METHODS(X) \
    BASETEXTURE_METHODS(X) X(17, GetLevelDesc) X(18, GetCubeMapSurface) X(19, LockRect) \
    X(20, UnlockRect) X(21, AddDirtyRect)
#define IDIRECT3DCUBETEXTURE9_COUNT 22

#define IDIRECT3DVOLUMETEXTURE9_METHODS(X) \
    BASETEXTURE_METHODS(X) X(17, GetLevelDesc) X(18, GetVolumeLevel) X(19, LockBox) \
    X(20, UnlockBox) X(21, AddDirtyBox)
#define IDIRECT3DVOLUMETEXTURE9_COUNT 22

#define IDIRECT3DVERTEXBUFFER9_METHODS(X) \
    RESOURCE_METHODS(X) X(11, Lock) X(12, Unlock) X(13, GetDesc)
#define IDIRECT3DVERTEXBUFFER9_COUNT 14

#define IDIRECT3DINDEXBUFFER9_METHODS(X) \
    RESOURCE_METHODS(X) X(11, Lock) X(12, Unlock) X(13, GetDesc)
#define IDIRECT3DINDEXBUFFER9_COUNT 14

#define IDIRECT3DSURFACE9_METHODS(X) \
    RESOURCE_METHODS(X) X(11, GetContainer) X(12, GetDesc) X(13, LockRect) X(14, UnlockRect) \
    X(15, GetDC) X(16, ReleaseDC)
#define IDIRECT3DSURFACE9_COUNT 17

#define IDIRECT3DQUERY9_METHODS(X) \
    X(0, QueryInterface) X(1, AddRef) X(2, Release) X(3, GetDevice) X(4, GetType) \
    X(5, GetDataSize) X(6, Issue) X(7, GetData)
#define IDIRECT3DQUERY9_COUNT 8

#define IDIRECT3DVERTEXSHADER9_METHODS(X) \
    X(0, QueryInterface) X(1, AddRef) X(2, Release) X(3, GetDevice) X(4, GetFunction)
#define IDIRECT3DVERTEXSHADER9_COUNT 5

#define IDIRECT3DPIXELSHADER9_METHODS(X) \
    X(0, QueryInterface) X(1, AddRef) X(2, Release) X(3, GetDevice) X(4, GetFunction)
#define IDIRECT3DPIXELSHADER9_COUNT 5

#define IDIRECT3DVERTEXDECLARATION9_METHODS(X) \
    X(0, QueryInterface) X(1, AddRef) X(2, Release) X(3, GetDevice) X(4, GetDeclaration)
#define IDIRECT3DVERTEXDECLARATION9_COUNT 5

// Interface ids: index into the proxy's interface table, first byte of every trace record,
// and the first macro argument of FOR_EACH_IFACE. Keep in sync with docs/TRACE_FORMAT.md.
#define IFACE_D3D9 0
#define IFACE_DEVICE 1
#define IFACE_SWAPCHAIN 2
#define IFACE_TEXTURE 3
#define IFACE_CUBE 4
#define IFACE_VOLUME 5
#define IFACE_VB 6
#define IFACE_IB 7
#define IFACE_SURFACE 8
#define IFACE_QUERY 9
#define IFACE_VSHADER 10
#define IFACE_PSHADER 11
#define IFACE_VDECL 12
#define IFACE_COUNT 13
#define IFACE_MAX_SLOTS 119

// F(id, symbol, COM interface, method-list macro, count)
#define FOR_EACH_IFACE(F) \
    F(0, d3d9, IDirect3D9, IDIRECT3D9_METHODS, IDIRECT3D9_COUNT) \
    F(1, device, IDirect3DDevice9, IDIRECT3DDEVICE9_METHODS, IDIRECT3DDEVICE9_COUNT) \
    F(2, swapchain, IDirect3DSwapChain9, IDIRECT3DSWAPCHAIN9_METHODS, IDIRECT3DSWAPCHAIN9_COUNT) \
    F(3, texture, IDirect3DTexture9, IDIRECT3DTEXTURE9_METHODS, IDIRECT3DTEXTURE9_COUNT) \
    F(4, cube, IDirect3DCubeTexture9, IDIRECT3DCUBETEXTURE9_METHODS, IDIRECT3DCUBETEXTURE9_COUNT) \
    F(5, volume, IDirect3DVolumeTexture9, IDIRECT3DVOLUMETEXTURE9_METHODS, IDIRECT3DVOLUMETEXTURE9_COUNT) \
    F(6, vb, IDirect3DVertexBuffer9, IDIRECT3DVERTEXBUFFER9_METHODS, IDIRECT3DVERTEXBUFFER9_COUNT) \
    F(7, ib, IDirect3DIndexBuffer9, IDIRECT3DINDEXBUFFER9_METHODS, IDIRECT3DINDEXBUFFER9_COUNT) \
    F(8, surface, IDirect3DSurface9, IDIRECT3DSURFACE9_METHODS, IDIRECT3DSURFACE9_COUNT) \
    F(9, query, IDirect3DQuery9, IDIRECT3DQUERY9_METHODS, IDIRECT3DQUERY9_COUNT) \
    F(10, vshader, IDirect3DVertexShader9, IDIRECT3DVERTEXSHADER9_METHODS, IDIRECT3DVERTEXSHADER9_COUNT) \
    F(11, pshader, IDirect3DPixelShader9, IDIRECT3DPIXELSHADER9_METHODS, IDIRECT3DPIXELSHADER9_COUNT) \
    F(12, vdecl, IDirect3DVertexDeclaration9, IDIRECT3DVERTEXDECLARATION9_METHODS, IDIRECT3DVERTEXDECLARATION9_COUNT)
