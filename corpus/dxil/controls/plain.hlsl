// The negative control: same shape, no wave ops.
RWStructuredBuffer<uint> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Out[tid.x] = tid.x * 3 + 1;
}
