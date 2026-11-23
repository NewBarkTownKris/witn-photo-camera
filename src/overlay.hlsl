// Straight-alpha finder bitmap composited over the presented game image.
struct Vertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
Vertex vs_main(uint id : SV_VertexID) {
    Vertex output;
    output.uv=float2((id<<1)&2,id&2);
    output.position=float4(output.uv*float2(2,-2)+float2(-1,1),0,1);
    return output;
}
Texture2D<float4> finder : register(t0);
SamplerState sampler0 : register(s0);
float4 ps_main(Vertex v) : SV_Target {
    return finder.Sample(sampler0,v.uv);
}
