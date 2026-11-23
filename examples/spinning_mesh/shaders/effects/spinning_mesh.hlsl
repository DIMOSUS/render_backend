cbuffer Spin
{
    float angle;
    float aspect;
};

struct Vertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

float3 rotate(float3 p)
{
    float s, c;
    sincos(angle, s, c);
    p = float3(c * p.x + s * p.z, p.y, c * p.z - s * p.x);
    // A slight downward view of the model.
    return float3(p.x, 0.9925 * p.y + 0.122 * p.z, 0.9925 * p.z - 0.122 * p.y);
}

VertexOutput VS_Main(Vertex vertex)
{
    float3 view = rotate(vertex.position - float3(0, 0.29, 0));
    view.z += 3.65;
    const float focalLength = 2.26;
    const float nearPlane = 0.1;
    const float farPlane = 30.0;
    const float depthScale = farPlane / (farPlane - nearPlane);

    VertexOutput output;
    output.position = float4(view.x * focalLength / aspect, view.y * focalLength,
                             (view.z - nearPlane) * depthScale, view.z);
    output.normal = rotate(vertex.normal);
    output.color = vertex.color;
    return output;
}

float4 PS_Main(VertexOutput vertex) : SV_Target
{
    const float3 normal = normalize(vertex.normal);
    const float diffuse = saturate(dot(normal, normalize(float3(-0.6, 0.85, -0.65))));
    const float edge = saturate(dot(normal, normalize(float3(0.65, 0.3, 0.72))));
    const float3 color = vertex.color * (0.38 + 0.60 * diffuse) + float3(0.07, 0.11, 0.14) * edge;
    return float4(color, 1);
}

technique Default
{
    pass Main
    {
        VertexShader = VS_Main();
        PixelShader = PS_Main();
        CullMode = NONE;
        Blend = NONE;
        DepthTest = true;
        DepthWrite = true;
    }
}
