struct PS_INPUT
{
    float4 Pos : SV_POSITION;
};

float4 main_ps(PS_INPUT input) : SV_Target
{
    return float4(0.9, 0.9, 0.9, 1.0);
}