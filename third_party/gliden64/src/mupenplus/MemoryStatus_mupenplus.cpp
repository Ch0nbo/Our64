#include "../MemoryStatus.h"
#include "../FrameBuffer.h"

bool isMemoryWritable(void * ptr, size_t byteCount)
{
	// Our64: the probe-overlap switch keeps the decoder out of RDRAM.
	return our64_rdram_writes != 0;
}
