// Work graphs (P50, P62), D3D12 only: the entry node gets one record with a
// multiplier and sends one record per thread (64) to `work`, which adds its
// value to output[0]. Expected sum: multiplier * (0 + 1 + ... + 63).
#include <equine/root.hlsli>

struct GraphRoot {
    uint outputSlot;
};
EQ_ROOT_DATA(GraphRoot, root);

struct EntryRecord {
    uint multiplier;
};

struct WorkRecord {
    uint value;
};

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
void entry(DispatchNodeInputRecord<EntryRecord> input, uint thread : SV_GroupThreadID,
           [MaxRecords(64)] NodeOutput<WorkRecord> work) {
    ThreadNodeOutputRecords<WorkRecord> record = work.GetThreadNodeOutputRecords(1);
    record.Get().value = thread * input.Get().multiplier;
    record.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void work(ThreadNodeInputRecord<WorkRecord> input) {
    RWByteAddressBuffer output = ResourceDescriptorHeap[root.outputSlot];
    output.InterlockedAdd(0, input.Get().value);
}
