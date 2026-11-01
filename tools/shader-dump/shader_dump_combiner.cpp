// Combiner.cpp built into shader-dump, with its static Combiner_GetStages
// reachable from shader_dump.cpp.
#include <Combiner.cpp>

void our64_combiner_stages(const CombinerKey & _key, Combiner & _color, Combiner & _alpha)
{
	Combiner_GetStages(_key, _color, _alpha);
}
