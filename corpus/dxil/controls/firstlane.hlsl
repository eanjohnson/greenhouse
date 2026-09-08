RWStructuredBuffer<uint> Out : register(u0);
[numthreads(64,1,1)] void main(uint3 t : SV_DispatchThreadID){ Out[t.x] = WaveIsFirstLane() ? t.x : 0; }
