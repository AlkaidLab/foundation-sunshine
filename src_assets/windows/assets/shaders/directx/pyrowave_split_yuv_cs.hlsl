// Split the local NV12/P010 conversion result into three independently
// shareable planes. D3D11 cannot reliably export a planar NV12/P010 texture
// as an NT handle on every WDDM driver, while R8/R16 textures are supported.
Texture2D<float> gY : register(t0);
Texture2D<float2> gUV : register(t1);
RWTexture2D<float> gOutY : register(u0);
RWTexture2D<float> gOutU : register(u1);
RWTexture2D<float> gOutV : register(u2);

[numthreads(16, 16, 1)]
void main_cs(uint3 id : SV_DispatchThreadID) {
  uint width;
  uint height;
  gOutY.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;

  gOutY[id.xy] = gY.Load(int3(id.xy, 0));
  if ((id.x & 1u) == 0u && (id.y & 1u) == 0u) {
    const float2 chroma = gUV.Load(int3(id.x / 2u, id.y / 2u, 0));
    gOutU[id.xy / 2u] = chroma.x;
    gOutV[id.xy / 2u] = chroma.y;
  }
}
