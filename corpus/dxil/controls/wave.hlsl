// #175 control: the smallest possible SM 6.5 wave-op compute shader. No heap
// handles, no lowering, nothing of ours in it -- straight out of dxc at cs_6_5.
// If this kills the console's back end, wave ops are a back-end limit and our
// lowering is innocent.
RWStructuredBuffer<uint> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint v = tid.x;
    uint s = WaveActiveSum(v);
    s += WavePrefixSum(v);
    s += WaveActiveMax(v);
    if (WaveIsFirstLane()) s += WaveGetLaneCount();
    Out[tid.x] = s;
}
