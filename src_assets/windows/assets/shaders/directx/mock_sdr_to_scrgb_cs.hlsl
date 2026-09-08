Texture2D<float4> input_texture : register(t0);
RWTexture2D<float4> output_texture : register(u0);

float3 srgb_to_linear(float3 value) {
  const float3 low = value / 12.92f;
  const float3 high = pow((value + 0.055f) / 1.055f, 2.4f);
  const float3 low_mask = 1.0f - step(0.04045f, value);
  return lerp(high, low, low_mask);
}

[numthreads(16, 16, 1)]
void main_cs(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint width;
  uint height;
  input_texture.GetDimensions(width, height);
  if (dispatch_thread_id.x >= width || dispatch_thread_id.y >= height) {
    return;
  }

  const float4 source = input_texture.Load(uint3(dispatch_thread_id.xy, 0));
  output_texture[dispatch_thread_id.xy] = float4(srgb_to_linear(source.rgb), 1.0f);
}
