cbuffer UploadConstants : register(b0)
{
	uint sourceIndex;
	uint targetsBase;
	uint wordCount;
	uint rowWidth;
};

[numthreads(64, 1, 1)]
void ScatterUpload(uint2 thread : SV_DispatchThreadID)
{
	const uint index = thread.x + thread.y * rowWidth;
	if (index >= wordCount)
		return;
	ByteAddressBuffer source = ResourceDescriptorHeap[sourceIndex];
	const uint address = index * 20;
	const uint remap = source.Load(address);
	RWByteAddressBuffer target = ResourceDescriptorHeap[NonUniformResourceIndex(targetsBase + (remap >> 28))];
	const uint offset = remap & 0x0ffffffcu;
	const uint4 value = source.Load4(address + 4);
	switch (remap & 3)
	{
	case 0: target.Store(offset, value.x); break;
	case 1: target.Store2(offset, value.xy); break;
	case 2: target.Store3(offset, value.xyz); break;
	default: target.Store4(offset, value); break;
	}
}
