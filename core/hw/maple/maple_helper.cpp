#include "maple_helper.h"
#include "maple_if.h"

u32 maple_GetAttachedDevices(u32 bus)
{
	verify(MapleDevices[bus][5]!=0);

	u32 rv=0;
	
	for (int i=0;i<5;i++)
		rv|=(MapleDevices[bus][i]!=0?1:0)<<i;

	return rv;
}
