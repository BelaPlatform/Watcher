#include <Watcher.h>
Watcher<float> myvar("myvar");
Watcher<float> myvar2("myvar2");

#include <Bela.h>
#include <cmath>

float gFrequency = 440.0;
float gPhase;
float gInverseSampleRate;

bool setup(BelaContext *context, void *userData)
{
	gui.setup(context->projectName);
	gInverseSampleRate = 1.0 / context->audioSampleRate;
	gPhase = 0.0;
	return true;
}

void render(BelaContext *context, void *userData)
{
	Bela_getDefaultWatcherManager()->tickBlock(context->audioFramesElapsed);
	myvar = fmodf(context->audioFramesElapsed / (3 * context->audioSampleRate), 1);
	myvar2 = fmodf(context->audioFramesElapsed / (3 * context->audioSampleRate), 1);
	static size_t count = 0;
	if(count++ >= context->audioSampleRate * 0.6 / context->audioFrames)
	{
		rt_printf("%.5f %.5f\n\r", float(myvar), float(myvar2));
		static int pastC = -1;
		int c = gui.numConnections();
		if(c != pastC)
			rt_printf("connected %d\n", c);
		pastC = c;
		count = 0;
	}

	for(unsigned int n = 0; n < context->audioFrames; n++) {
		float out = 0.8 * sinf(gPhase);
		gPhase += 2.0 * M_PI * gFrequency * gInverseSampleRate;
		if(gPhase > 2.0 * M_PI)
			gPhase -= 2.0 * M_PI;

		for(unsigned int channel = 0; channel < context->audioOutChannels; channel++) {
			audioWrite(context, n, channel, out);
		}
	}
}

void cleanup(BelaContext *context, void *userData)
{
}
