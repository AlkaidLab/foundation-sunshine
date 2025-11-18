cbuffer TransformBuffer : register(b0) {
    float2 position; // x, y
    float2 screenSize; // w, h
};

struct PS_INPUT
{
    float4 Pos : SV_POSITION;
};


PS_INPUT main_vs(uint vertexID: SV_VERTEXID) {
    PS_INPUT output;
    float xOff = position.x / screenSize.x * 2 - 1;
    float yOff = position.y / screenSize.y * 2 - 1;

    if (vertexID == 0) {
        output.Pos = float4(xOff, yOff, 0.0, 1.0);
    } else if (vertexID == 1) {
        output.Pos = float4(xOff + 22 / screenSize.x, yOff + 22 / screenSize.y, 0.0, 1.0);
    } else {
        output.Pos = float4(xOff, yOff + 32 / screenSize.y, 0.0, 1.0);
    }

    return output;
}