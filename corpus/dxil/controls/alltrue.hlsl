RWStructuredBuffer<uint> Out : register(u0);
[numthreads(64,1,1)] void main(uint3 t : SV_DispatchThreadID){ Out[t.x] = WaveActiveAllTrue(t.x > 3) ? 1 : 0; }
