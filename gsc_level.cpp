#include "gsc_level.hpp"

#if COMPILE_LEVEL == 1

unsigned int getNumberOfStaticModels()
{
#if COD_VERSION == COD2_1_0
        int address = 0x08185BE4;
#elif COD_VERSION == COD2_1_2
        int address = 0x08187D44;
#elif COD_VERSION == COD2_1_3
        int address = 0x08188DC4;
#endif
	return *(unsigned int*)address;
}

void gsc_level_getnumstaticmodels()
{
	stackPushInt(getNumberOfStaticModels());
}

void gsc_level_getstaticmodel()
{
#if COD_VERSION == COD2_1_0
	int address = 0x08185BE8;
#elif COD_VERSION == COD2_1_2
	int address = 0x08187D48;
#elif COD_VERSION == COD2_1_3
	int address = 0x08188DC8;
#endif
	unsigned int numberOfStaticModels = getNumberOfStaticModels();
	int index;

	if (!stackGetParamInt(0, &index))
	{
		stackError("gsc_level_getstaticmodel() argument is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	if (index < 0 || index >= (int)numberOfStaticModels)
	{
		stackError("gsc_level_getstaticmodel() index is out of range");
		stackPushUndefined();
		return;
	}

	int staticModelAddress = *(int*)address + 80 * index;
	XModel_t* xmodel = (XModel_t*)*(int*)(staticModelAddress + 4);
	float* origin = (float*)(staticModelAddress + 8);

	stackPushArray();

	stackPushString(xmodel->name);
	stackPushArrayLast();

	stackPushVector(origin);
	stackPushArrayLast();
}

#endif
