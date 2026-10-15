RaytracingAccelerationStructure scene : register(t0);
struct InputRay { float3 origin; float minimum; float3 direction; float maximum; };
StructuredBuffer<InputRay> rays : register(t1);
struct Hit { float distance; uint primitive; float2 barycentric; };
RWStructuredBuffer<Hit> hits : register(u0);
cbuffer DispatchInfo : register(b0) { uint count; };
[numthreads(64, 1, 1)]
void Main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    InputRay input = rays[id.x];
    RayDesc ray;
    ray.Origin = input.origin; ray.TMin = input.minimum;
    ray.Direction = input.direction; ray.TMax = input.maximum;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 255, ray);
    while (query.Proceed()) {}
    Hit hit; hit.distance = -1; hit.primitive = 0xffffffff; hit.barycentric = 0;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        hit.distance = query.CommittedRayT();
        hit.primitive = query.CommittedPrimitiveIndex();
        hit.barycentric = query.CommittedTriangleBarycentrics();
    }
    hits[id.x] = hit;
}
