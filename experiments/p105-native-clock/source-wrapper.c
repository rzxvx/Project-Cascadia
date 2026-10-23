/* Expose the candidate's actual fixed metadata for the pinned ADT oracle. */
#include "cascadia-native-clock.c"
unsigned int source_metadata(unsigned int n, unsigned int field)
{
	if (n >= CASCADIA_NATIVE_NODES || field >= 7)
		return ~0U;
	if (field == 0)
		return nodes[n].id;
	if (field == 1)
		return nodes[n].clock;
	if (field == 2)
		return nodes[n].gate;
	return nodes[n].parents[field - 3] == 0xff ? 0 :
		nodes[nodes[n].parents[field - 3]].id;
}
