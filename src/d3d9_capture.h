#pragma once

#include <d3d9.h>

namespace Capture
{
    void OnPresent();
    void OnShaderCreated(void* shader, const DWORD* code);
    void OnSetPixelShader(void* shader);
    void OnSetVertexShader(void* shader);
    void OnSetFVF(DWORD fvf);
    void OnSetTexture(DWORD stage, IDirect3DBaseTexture9* tex);
    void OnSetRenderState(D3DRENDERSTATETYPE state, DWORD value);
    void OnSetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value);
    void OnSetPixelShaderConstantF(UINT start, const float* data, UINT count);
    void OnSetRenderTarget(DWORD index, IDirect3DSurface9* rt);
    void OnClear(DWORD flags, D3DCOLOR color);
    void OnDraw(const char* kind, unsigned prim, unsigned count);
    bool Active();
}
