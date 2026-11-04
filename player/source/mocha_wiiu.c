/* Our64 player: Aroma's Mocha, asked directly over IOS (mocha_wiiu.h). Mocha
 * answers its API version on /dev/mcp and unlocks an FSA client with ioctl
 * 0x28, as its public documentation describes. */
#include "mocha_wiiu.h"

#include <coreinit/ios.h>

int mocha_wiiu_present(void)
{
	int mcp = IOS_Open("/dev/mcp", IOS_OPEN_READ);
	if (mcp < 0)
		return 0;
	uint32_t io[0x40 / 4] __attribute__((aligned(0x40)));
	io[0] = 0xF8; /* IPC_CUSTOM_GET_MOCHA_API_VERSION */
	int mocha = IOS_Ioctl(mcp, 100, io, 4, io, 8) == 0 && io[0] == 0xCAFEBABE && io[1] >= 1;
	IOS_Close(mcp);
	return mocha;
}

FSAClientHandle mocha_wiiu_fsa_client(void)
{
	if (!mocha_wiiu_present())
		return -1;
	FSAInit();
	FSAClientHandle c = FSAAddClient(NULL);
	if (c < 0)
		return -1;
	int dummy[0x40 / 4] __attribute__((aligned(0x40)));
	if (IOS_Ioctl(c, 0x28, dummy, sizeof(dummy), dummy, sizeof(dummy)) != 0) {
		FSADelClient(c);
		return -1;
	}
	return c;
}
