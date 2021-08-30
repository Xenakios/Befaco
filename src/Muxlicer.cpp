#include "plugin.hpp"

// an implementation of a performable, 3-stage switch, i.e. where
// the state triggers after being dragged a certain distance
struct BefacoSwitchMomentary : SVGSwitch {
	BefacoSwitchMomentary() {
		momentary = true;
		addFrame(APP->window->loadSvg(asset::system("res/ComponentLibrary/BefacoSwitch_0.svg")));
		addFrame(APP->window->loadSvg(asset::system("res/ComponentLibrary/BefacoSwitch_1.svg")));
		addFrame(APP->window->loadSvg(asset::system("res/ComponentLibrary/BefacoSwitch_2.svg")));
	}

	void onDragStart(const event::DragStart& e) override {
		latched = false;
		startMouseY = APP->scene->rack->mousePos.y;
		ParamWidget::onDragStart(e);
	}

	void onDragMove(const event::DragMove& e) override {

		float diff = APP->scene->rack->mousePos.y - startMouseY;

		// Once the user has dragged the mouse a "threshold" distance, latch
		// to disallow further changes of state until the mouse is released.
		// We don't just setValue(1) (default/rest state) because this creates a
		// jarring UI experience
		if (diff < -10 && !latched) {
			paramQuantity->setValue(2);
			latched = true;
		}
		if (diff > 10 && !latched) {
			paramQuantity->setValue(0);
			latched = true;
		}

		ParamWidget::onDragMove(e);
	}

	void onDragEnd(const event::DragEnd& e) override {
		// on release, the switch resets to default/neutral/middle position
		paramQuantity->setValue(1);
		latched = false;
		ParamWidget::onDragEnd(e);
	}

	// do nothing
	void randomize() override {}

	float startMouseY = 0.f;
	bool latched = false;
};


// Class which can yield a divided clock state, specifically where the
// gate is generated at request time through getGate(), rather than during
// process() - this means that different divisions of clock can be requested
// at any point in time. In contrast, the division/multiplication setting for
// ClockMultDiv cannot easily be changed _during_ a clock tick.
struct MultiGateClock {

	float remaining = 0.f;
	float fullPulseLength = 0.f;

	/** Immediately disables the pulse */
	void reset(float newfullPulseLength) {
		fullPulseLength = newfullPulseLength;
		remaining = fullPulseLength;
	}

	/** Advances the state by `deltaTime`. Returns whether the pulse is in the HIGH state. */
	bool process(float deltaTime) {
		if (remaining > 0.f) {
			remaining -= deltaTime;
			return true;
		}
		return false;
	}

	float getGate(int gateMode) {

		if (gateMode == 0) {
			// always on (special case)
			return 10.f;
		}
		else if (gateMode < 0 || remaining <= 0) {
			// disabled (or elapsed)
			return 0.f;
		}

		const float multiGateOnLength = fullPulseLength / ((gateMode > 0) ? (2.f * gateMode) : 1.0f);
		const bool isOddPulse = int(floor(remaining / multiGateOnLength)) % 2;

		return isOddPulse ? 10.f : 0.f;
	}
};


// Class for generating a clock sequence after setting a clock multiplication or division,
// given a stream of clock pulses as the "base" clock.
// Implementation is heavily inspired by BogAudio RGate, with modification
struct MultDivClock {

	// convention: negative values are used for division (1/mult), positive for multiplication (x mult)
	// multDiv = 0 should not be used, but if it is it will result in no modification to the clock
	int multDiv = 1;
	float secondsSinceLastClock = -1.0f;
	float inputClockLengthSeconds = -1.0f;

	// count how many divisions we've had
	int dividerCount = 0;

	float dividedProgressSeconds = 0.f;

	// returns the gated clock signal
	float process(float deltaTime, bool clockPulseReceived) {

		if (clockPulseReceived) {
			// update our record of the incoming clock spacing
			if (secondsSinceLastClock > 0.0f) {
				inputClockLengthSeconds = secondsSinceLastClock;
			}
			secondsSinceLastClock = 0.0f;
		}

		float out = 0.f;
		if (secondsSinceLastClock >= 0.0f) {
			secondsSinceLastClock += deltaTime;

			// negative values are used for division (x 1/mult), positive for multiplication (x mult)
			const int division = std::max(-multDiv, 1);
			const int multiplication = std::max(multDiv, 1);

			if (clockPulseReceived) {
				if (dividerCount < 1) {
					dividedProgressSeconds = 0.0f;
				}
				else {
					dividedProgressSeconds += deltaTime;
				}
				++dividerCount;
				if (dividerCount >= division) {
					dividerCount = 0;
				}
			}
			else {
				dividedProgressSeconds += deltaTime;
			}

			// lengths of the mult/div versions of the clock
			const float dividedSeconds = inputClockLengthSeconds * (float) division;
			const float multipliedSeconds = dividedSeconds / (float) multiplication;

			// length of the output gate (s)
			const float gateSeconds = std::max(0.001f, multipliedSeconds * 0.5f);

			if (dividedProgressSeconds < dividedSeconds) {
				float multipliedProgressSeconds = dividedProgressSeconds / multipliedSeconds;
				multipliedProgressSeconds -= (float)(int)multipliedProgressSeconds;
				multipliedProgressSeconds *= multipliedSeconds;
				out += (float)(multipliedProgressSeconds <= gateSeconds);
			}
		}
		return out;
	}

	float getEffectiveClockLength() {
		// negative values are used for division (x 1/mult), positive for multiplication (x mult)
		const int division = std::max(-multDiv, 1);
		const int multiplication = std::max(multDiv, 1);

		// lengths of the mult/div versions of the clock
		const float dividedSeconds = inputClockLengthSeconds * (float) division;
		const float multipliedSeconds = dividedSeconds / (float) multiplication;

		return multipliedSeconds;
	}
};

struct Muxlicer : Module {
	enum ParamIds {
		PLAY_PARAM,
		ADDRESS_PARAM,
		GATE_MODE_PARAM,
		TAP_TEMPO_PARAM,
		ENUMS(LEVEL_PARAMS, 8),
		NUM_PARAMS
	};
	enum InputIds {
		GATE_MODE_INPUT,
		ADDRESS_INPUT,
		CLOCK_INPUT,
		RESET_INPUT,
		COM_INPUT,
		ENUMS(MUX_INPUTS, 8),
		ALL_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		CLOCK_OUTPUT,
		ALL_GATES_OUTPUT,
		EOC_OUTPUT,
		ENUMS(GATE_OUTPUTS, 8),
		ENUMS(MUX_OUTPUTS, 8),
		COM_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		CLOCK_LIGHT,
		ENUMS(GATE_LIGHTS, 8),
		NUM_LIGHTS
	};

	enum ModeCOMIO {
		COM_1_IN_8_OUT,
		COM_8_IN_1_OUT
	};

	enum PlayState {
		STATE_PLAY_ONCE,
		STATE_STOPPED,
		STATE_PLAY
	};

	/*
	This shows how the values of the gate mode knob + CV map onto gate triggers.
	See also getGateMode()
	value   | description     	    | quadratic only mode
	   -1 	   no gate  	    	|	    ✔
	    0      gate (full timestep) |       x
	   +1      half timestep      	|	    ✔
	    2      two gates       	    | 	    ✔
	    3      three gates     	    |       x
	    4      four gates      	    |       ✔
	    5      five gates      	    |       x
	    6      six gates       	    |       x
	    7      seven gates     	    |       x
	    8      eight gates     	    |       ✔
	*/
	int possibleQuadraticGates[5] = {-1, 1, 2, 4, 8};
	bool quadraticGatesOnly = false;

	PlayState playState = STATE_STOPPED;	
	dsp::BooleanTrigger playStateTrigger;

	uint32_t runIndex; 	// which step are we on (0 to 7)
	uint32_t addressIndex = 0;
	bool reset = false;

	// used to track the clock (e.g. if external clock is not connected). NOTE: this clock
	// is defined _prior_ to any clock division/multiplication logic
	float internalClockProgress = 0.f;
	float internalClockLength = 0.25f;

	float tapTime = 99999;	// used to track the time between clock pulses (or taps?)
	dsp::SchmittTrigger inputClockTrigger;	// to detect incoming clock pulses 
	dsp::SchmittTrigger mainClockTrigger;	// to detect rising edges from the divided/multiplied version of the clock signal
	dsp::SchmittTrigger resetTrigger; 		// to detect the reset signal
	dsp::PulseGenerator endOfCyclePulse; 	// fire a signal at the end of cycle
	dsp::BooleanTrigger tapTempoTrigger;	// to only trigger tap tempo when push is first detected

	MultDivClock mainClockMultDiv;			// to produce a divided/multiplied version of the (internal or external) clock signal
	MultDivClock outputClockMultDiv;		// to produce a divided/multiplied version of the output clock signal
	MultiGateClock multiClock;				// to easily produce a divided version of the main clock (where division can be changed at any point)

	const static int SEQUENCE_LENGTH = 8;
	ModeCOMIO modeCOMIO = COM_1_IN_8_OUT;	// are we in 1-in-8-out mode, or 8-in-1-out mode
	int allInNormalVoltage = 10;			// what voltage is normalled into the "All In" input, selectable via context menu
	Module* rightModule;					// for the expander

	Muxlicer() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(Muxlicer::PLAY_PARAM, STATE_PLAY_ONCE, STATE_PLAY, STATE_STOPPED, "Play switch");
		configParam(Muxlicer::ADDRESS_PARAM, -1.f, 7.f, -1.f, "Address");
		configParam(Muxlicer::GATE_MODE_PARAM, -1.f, 8.f, 0.f, "Gate mode");
		configParam(Muxlicer::TAP_TEMPO_PARAM, 0.f, 1.f, 0.f, "Tap tempo");

		for (int i = 0; i < SEQUENCE_LENGTH; ++i) {
			configParam(Muxlicer::LEVEL_PARAMS + i, 0.0, 1.0, 1.0, string::f("Slider %d", i));
		}

		onReset();
	}

	void onReset() override {
		internalClockLength = 0.250f;
		internalClockProgress = 0;
		runIndex = 0;
	}

	void process(const ProcessArgs& args) override {

		const bool usingExternalClock = inputs[CLOCK_INPUT].isConnected();

		bool externalClockPulseReceived = false;
		// a clock pulse does two things: sets the internal clock (based on timing between two pulses), and
		// also synchronises the clock
		if (usingExternalClock && inputClockTrigger.process(rescale(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.f, 0.f, 1.f))) {
			externalClockPulseReceived = true;
		}
		else if (!usingExternalClock && tapTempoTrigger.process(params[TAP_TEMPO_PARAM].getValue())) {
			externalClockPulseReceived = true;
		}

		if (resetTrigger.process(rescale(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f, 0.f, 1.f))) {
			reset = true;
			if (playState == STATE_STOPPED) {
				playState = STATE_PLAY_ONCE;
			}
		}

		processPlayResetSwitch();

		// TODO: work out CV scaling/conversion for ADDRESS_INPUT
		const float address = params[ADDRESS_PARAM].getValue() + inputs[ADDRESS_INPUT].getVoltage();
		const bool isSequenceAdvancing = address < 0.f;

		// even if we have an external clock, use its pulses to time/sync the internal clock
		// so that it will remain running after CLOCK_INPUT is disconnected
		if (externalClockPulseReceived) {
			// TODO: only want 2.f for tap for tempo, not all external clock
			if (tapTime < 2.f) {
				internalClockLength = tapTime;
			}
			tapTime = 0;
			internalClockProgress = 0;
		}
		tapTime += args.sampleTime;
		internalClockProgress += args.sampleTime;

		// track if the internal clock has "ticked"
		const bool internalClockPulseReceived = (internalClockProgress >= internalClockLength);
		if (internalClockPulseReceived) {
			internalClockProgress = 0.f;
		}

		// we can be in one of two clock modes:
		// * external (decided by pulses to CLOCK_INPUT)
		// * internal (decided by internalClockProgress exceeding the internal clock length)
		//
		// choose which clock source we are to use
		const bool clockPulseReceived = usingExternalClock ? externalClockPulseReceived : internalClockPulseReceived;
		// apply the main clock div/mult logic to whatever clock source we're using - this outputs a gate sequence
		// so we must use a Schmitt Trigger on the divided/mult'd signal in order to detect when to advance the sequence
		const bool dividedMultedClockPulseReceived = mainClockTrigger.process(mainClockMultDiv.process(args.sampleTime, clockPulseReceived));

		// reset _doesn't_ reset/sync the clock, it just moves the sequence index marker back to the start
		if (reset) {
			runIndex = 0;
			reset = false;
		}

		// end of cycle trigger trigger
		outputs[EOC_OUTPUT].setVoltage(0.f);

		if (dividedMultedClockPulseReceived) {

			if (isSequenceAdvancing) {
				runIndex++;
				if (runIndex >= 8) {
					// both play modes will reset to step 0 and fire an EOC trigger
					runIndex = 0;
					endOfCyclePulse.trigger(1e-3);

					// additionally stop if in one shot mode
					if (playState == STATE_PLAY_ONCE) {
						playState = STATE_STOPPED;
					}
				}
			}

			multiClock.reset(mainClockMultDiv.getEffectiveClockLength());

			for (int i = 0; i < 8; i++) {
				outputs[GATE_OUTPUTS + i].setVoltage(0.f);
			}
		}

		if (isSequenceAdvancing) {
			addressIndex = runIndex;
		}
		else {
			addressIndex = clamp((int) roundf(address), 0, 8 - 1);
		}

		// Gates
		for (int i = 0; i < 8; i++) {
			outputs[GATE_OUTPUTS + i].setVoltage(0.f);
			lights[GATE_LIGHTS + i].setBrightness(0.f);
		}
		outputs[ALL_GATES_OUTPUT].setVoltage(0.f);

		multiClock.process(args.sampleTime);
		const int gateMode = getGateMode();

		if (playState != STATE_STOPPED) {
			// current gate output _and_ "All Gates" output get the gate pattern from multiClock
			float gateValue = multiClock.getGate(gateMode);
			outputs[GATE_OUTPUTS + addressIndex].setVoltage(gateValue);
			lights[GATE_LIGHTS + addressIndex].setBrightness(gateValue / 10.f);
			outputs[ALL_GATES_OUTPUT].setVoltage(gateValue);
		}


		if (modeCOMIO == COM_1_IN_8_OUT) {
			// Mux outputs (all zero, except active step, if playing)
			for (int i = 0; i < 8; i++) {
				outputs[MUX_OUTPUTS + i].setVoltage(0.f);
			}

			if (playState != STATE_STOPPED) {
				const float com_input = inputs[COM_INPUT].getVoltage();
				const float stepVolume = params[LEVEL_PARAMS + addressIndex].getValue();
				outputs[MUX_OUTPUTS + addressIndex].setVoltage(stepVolume * com_input);
			}
		}
		else if (modeCOMIO == COM_8_IN_1_OUT && playState != STATE_STOPPED) {
			const float allInValue = inputs[ALL_INPUT].getNormalVoltage(allInNormalVoltage);
			const float stepVolume = params[LEVEL_PARAMS + addressIndex].getValue();
			float stepValue = inputs[MUX_INPUTS + addressIndex].getNormalVoltage(allInValue) * stepVolume;
			outputs[COM_OUTPUT].setVoltage(stepValue);
		}

		const bool isOutputClockHigh = outputClockMultDiv.process(args.sampleTime, clockPulseReceived);
		outputs[CLOCK_OUTPUT].setVoltage(isOutputClockHigh ? 10.f : 0.f);
		lights[CLOCK_LIGHT].setBrightness(isOutputClockHigh ? 1.f : 0.f);
		outputs[EOC_OUTPUT].setVoltage(endOfCyclePulse.process(args.sampleTime) ? 10.f : 0.f);

		if (rightExpander.module && rightExpander.module->model == modelMex) {
			// Get message from right expander
			MexMessage* message = (MexMessage*) rightExpander.module->leftExpander.producerMessage;

			// Write message
			message->addressIndex = addressIndex;
			message->allGates = multiClock.getGate(gateMode);
			message->outputClock = isOutputClockHigh ? 10.f : 0.f;
			message->isPlaying = (playState != STATE_STOPPED);

			// Flip messages at the end of the timestep
			rightExpander.module->leftExpander.messageFlipRequested = true;
		}
	}

	void processPlayResetSwitch() {

		// if the play switch has effectively been activated for the first time,
		// i.e. it's not just still being held
		const bool switchIsActive = params[PLAY_PARAM].getValue() != STATE_STOPPED;
		if (playStateTrigger.process(switchIsActive) && switchIsActive) {

			// if we were stopped, check for activation (normal or one-shot)
			if (playState == STATE_STOPPED) {
				if (params[PLAY_PARAM].getValue() == STATE_PLAY) {
					playState = STATE_PLAY;
				}
				else if (params[PLAY_PARAM].getValue() == STATE_PLAY_ONCE) {
					playState = STATE_PLAY_ONCE;
					runIndex = 0;
					reset = true;
				}
			}
			// otherwise we are in play mode (and we've not just held onto the play switch),
			// so check for stop or reset
			else {

				// top switch will stop
				if (params[PLAY_PARAM].getValue() == STATE_PLAY) {
					playState = STATE_STOPPED;
				}
				// bottom will reset
				else if (params[PLAY_PARAM].getValue() == STATE_PLAY_ONCE) {
					reset = true;
					runIndex = 0;
				}
			}
		}
	}

	int getGateMode() {

		float gate;

		if (inputs[GATE_MODE_INPUT].isConnected()) {
			float gateCV = clamp(inputs[GATE_MODE_INPUT].getVoltage(), 0.f, 5.f) / 5.f;
			float knobAttenuation = rescale(params[GATE_MODE_PARAM].getValue(), -1.f, 8.f, 0.f, 1.f);
			// todo: check against hardware
			gate = rescale(gateCV * knobAttenuation, 0.f, 1.f, -1.0f, 8.f);
		}
		else {
			gate = params[GATE_MODE_PARAM].getValue();
		}

		if (quadraticGatesOnly) {
			int quadraticGateIndex = int(floor(rescale(gate, -1.f, 8.f, 0.f, 4.99f)));
			return possibleQuadraticGates[quadraticGateIndex];
		}
		else {
			return clamp((int) roundf(gate), -1, 8);
		}
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "modeCOMIO", json_integer(modeCOMIO));
		json_object_set_new(rootJ, "quadraticGatesOnly", json_boolean(quadraticGatesOnly));
		json_object_set_new(rootJ, "allInNormalVoltage", json_integer(allInNormalVoltage));
		json_object_set_new(rootJ, "mainClockMultDiv", json_integer(mainClockMultDiv.multDiv));
		json_object_set_new(rootJ, "outputClockMultDiv", json_integer(outputClockMultDiv.multDiv));
		json_object_set_new(rootJ, "playState", json_integer(playState));

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "modeCOMIO");
		modeCOMIO = (Muxlicer::ModeCOMIO) json_integer_value(modeJ);

		json_t* quadraticJ = json_object_get(rootJ, "quadraticGatesOnly");
		quadraticGatesOnly = json_boolean_value(quadraticJ);

		json_t* allInNormalVoltageJ = json_object_get(rootJ, "allInNormalVoltage");
		allInNormalVoltage = json_integer_value(allInNormalVoltageJ);

		json_t* mainClockMultDivJ = json_object_get(rootJ, "mainClockMultDiv");
		mainClockMultDiv.multDiv = json_integer_value(mainClockMultDivJ);

		json_t* outputClockMultDivJ = json_object_get(rootJ, "outputClockMultDiv");
		outputClockMultDiv.multDiv = json_integer_value(outputClockMultDivJ);

		json_t* playStateJ = json_object_get(rootJ, "playState");
		playState = (PlayState) json_integer_value(playStateJ);
	}

};


struct MuxlicerWidget : ModuleWidget {
	MuxlicerWidget(Muxlicer* module) {

		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Muxlicer.svg")));

		addChild(createWidget<Knurlie>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<Knurlie>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<Knurlie>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<Knurlie>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParam<BefacoSwitchMomentary>(mm2px(Vec(35.72963, 10.008)), module, Muxlicer::PLAY_PARAM));
		addParam(createParam<BefacoTinyKnobSnap>(mm2px(Vec(3.84112, 10.90256)), module, Muxlicer::ADDRESS_PARAM));
		addParam(createParam<BefacoTinyKnobWhite>(mm2px(Vec(67.83258, 10.86635)), module, Muxlicer::GATE_MODE_PARAM));
		addParam(createParam<BefacoButton>(mm2px(Vec(28.12238, 24.62151)), module, Muxlicer::TAP_TEMPO_PARAM));
		addParam(createParam<BefacoSlidePot>(mm2px(Vec(2.32728, 40.67102)), module, Muxlicer::LEVEL_PARAMS + 0));
		addParam(createParam<BefacoSlidePot>(mm2px(Vec(12.45595, 40.67102)), module, Muxlicer::LEVEL_PARAMS + 1));
		addParam(createParam<BefacoSlidePot>(mm2px(Vec(22.58462, 40.67102)), module, Muxlicer::LEVEL_PARAMS + 2));
		addParam(createParam<BefacoSlidePot>(mm2px(Vec(32.7133, 40.67102)), module, Muxlicer::LEVEL_PARAMS + 3));
		addParam(createParam<BefacoSlidePot>(mm2px(Vec(42.74195, 40.67102)), module, Muxlicer::LEVEL_PARAMS + 4));
		addParam(createParam<BefacoSlidePot>(mm2px(Vec(52.97062, 40.67102)), module, Muxlicer::LEVEL_PARAMS + 5));
		addParam(createParam<BefacoSlidePot>(mm2px(Vec(63.0993, 40.67102)), module, Muxlicer::LEVEL_PARAMS + 6));
		addParam(createParam<BefacoSlidePot>(mm2px(Vec(73.22797, 40.67102)), module, Muxlicer::LEVEL_PARAMS + 7));

		addInput(createInput<BefacoInputPort>(mm2px(Vec(51.568, 11.20189)), module, Muxlicer::GATE_MODE_INPUT));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(21.13974, 11.23714)), module, Muxlicer::ADDRESS_INPUT));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(44.24461, 24.93662)), module, Muxlicer::CLOCK_INPUT));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(12.62135, 24.95776)), module, Muxlicer::RESET_INPUT));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(36.3142, 98.07911)), module, Muxlicer::COM_INPUT));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(0.895950, 109.27901)), module, Muxlicer::MUX_INPUTS + 0));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(11.05332, 109.29256)), module, Muxlicer::MUX_INPUTS + 1));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(21.18201, 109.29256)), module, Muxlicer::MUX_INPUTS + 2));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(31.27625, 109.27142)), module, Muxlicer::MUX_INPUTS + 3));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(41.40493, 109.27142)), module, Muxlicer::MUX_INPUTS + 4));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(51.53360, 109.27142)), module, Muxlicer::MUX_INPUTS + 5));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(61.69671, 109.29256)), module, Muxlicer::MUX_INPUTS + 6));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(71.82537, 109.29256)), module, Muxlicer::MUX_INPUTS + 7));
		addInput(createInput<BefacoInputPort>(mm2px(Vec(16.11766, 98.09121)), module, Muxlicer::ALL_INPUT));

		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(59.8492, 24.95776)), module, Muxlicer::CLOCK_OUTPUT));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(56.59663, 98.06252)), module, Muxlicer::ALL_GATES_OUTPUT));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(66.72661, 98.07008)), module, Muxlicer::EOC_OUTPUT));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(0.89595, 86.78581)), module, Muxlicer::GATE_OUTPUTS + 0));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(11.02463, 86.77068)), module, Muxlicer::GATE_OUTPUTS + 1));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(21.14758, 86.77824)), module, Muxlicer::GATE_OUTPUTS + 2));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(31.27625, 86.77824)), module, Muxlicer::GATE_OUTPUTS + 3));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(41.40493, 86.77824)), module, Muxlicer::GATE_OUTPUTS + 4));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(51.56803, 86.79938)), module, Muxlicer::GATE_OUTPUTS + 5));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(61.69671, 86.79938)), module, Muxlicer::GATE_OUTPUTS + 6));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(71.79094, 86.77824)), module, Muxlicer::GATE_OUTPUTS + 7));

		// these blocks are exclusive (for visibility / interactivity) and allows IO and OI within one module
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(0.895950, 109.27901)), module, Muxlicer::MUX_OUTPUTS + 0));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(11.05332, 109.29256)), module, Muxlicer::MUX_OUTPUTS + 1));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(21.18201, 109.29256)), module, Muxlicer::MUX_OUTPUTS + 2));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(31.27625, 109.27142)), module, Muxlicer::MUX_OUTPUTS + 3));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(41.40493, 109.27142)), module, Muxlicer::MUX_OUTPUTS + 4));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(51.53360, 109.27142)), module, Muxlicer::MUX_OUTPUTS + 5));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(61.69671, 109.29256)), module, Muxlicer::MUX_OUTPUTS + 6));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(71.82537, 109.29256)), module, Muxlicer::MUX_OUTPUTS + 7));
		addOutput(createOutput<BefacoOutputPort>(mm2px(Vec(36.3142, 98.07911)), module, Muxlicer::COM_OUTPUT));

		updatePortVisibilityForIOMode(Muxlicer::COM_1_IN_8_OUT);

		addChild(createLight<SmallLight<RedLight>>(mm2px(Vec(71.28361, 28.02644)), module, Muxlicer::CLOCK_LIGHT));
		addChild(createLight<SmallLight<RedLight>>(mm2px(Vec(3.99336, 81.86801)), module, Muxlicer::GATE_LIGHTS + 0));
		addChild(createLight<SmallLight<RedLight>>(mm2px(Vec(14.09146, 81.86801)), module, Muxlicer::GATE_LIGHTS + 1));
		addChild(createLight<SmallLight<RedLight>>(mm2px(Vec(24.22525, 81.86801)), module, Muxlicer::GATE_LIGHTS + 2));
		addChild(createLight<SmallLight<RedLight>>(mm2px(Vec(34.35901, 81.86801)), module, Muxlicer::GATE_LIGHTS + 3));
		addChild(createLight<SmallLight<RedLight>>(mm2px(Vec(44.49277, 81.86801)), module, Muxlicer::GATE_LIGHTS + 4));
		addChild(createLight<SmallLight<RedLight>>(mm2px(Vec(54.62652, 81.86801)), module, Muxlicer::GATE_LIGHTS + 5));
		addChild(createLight<SmallLight<RedLight>>(mm2px(Vec(64.76028, 81.86801)), module, Muxlicer::GATE_LIGHTS + 6));
		addChild(createLight<SmallLight<RedLight>>(mm2px(Vec(74.89404, 81.86801)), module, Muxlicer::GATE_LIGHTS + 7));
	}

	void draw(const DrawArgs& args) override {
		Muxlicer* module = dynamic_cast<Muxlicer*>(this->module);

		if (module != nullptr) {
			updatePortVisibilityForIOMode(module->modeCOMIO);
		}
		else {
			// module can be null, e.g. if populating the module browser with screenshots,
			// in which case just assume the default (1 in, 8 out)
			updatePortVisibilityForIOMode(Muxlicer::COM_1_IN_8_OUT);
		}

		ModuleWidget::draw(args);
	}

	struct IOMenuItem : MenuItem {
		Muxlicer* module;
		MuxlicerWidget* widget;
		void onAction(const event::Action& e) override {
			module->modeCOMIO = Muxlicer::COM_1_IN_8_OUT;
			widget->updatePortVisibilityForIOMode(module->modeCOMIO);
			widget->clearCables();
		}
	};
	struct OIMenuItem : MenuItem {
		Muxlicer* module;
		MuxlicerWidget* widget;
		void onAction(const event::Action& e) override {
			module->modeCOMIO = Muxlicer::COM_8_IN_1_OUT;
			widget->updatePortVisibilityForIOMode(module->modeCOMIO);
			widget->clearCables();
		}
	};

	struct OutputRangeChildItem : MenuItem {
		Muxlicer* module;
		int allInNormalVoltage;
		void onAction(const event::Action& e) override {
			module->allInNormalVoltage = allInNormalVoltage;
		}
	};

	struct OutputRangeItem : MenuItem {
		Muxlicer* module;

		Menu* createChildMenu() override {
			Menu* menu = new Menu;

			std::vector<int> voltageOptions = {1, 5, 10};
			for (auto voltageOption : voltageOptions) {
				OutputRangeChildItem* rangeItem = createMenuItem<OutputRangeChildItem>(std::to_string(voltageOption) + "V",
				                                  CHECKMARK(module->allInNormalVoltage == voltageOption));
				rangeItem->allInNormalVoltage = voltageOption;
				rangeItem->module = module;
				menu->addChild(rangeItem);
			}

			return menu;
		}
	};

	static std::vector<int> getClockOptions() {
		return std::vector<int> {-16, -8, -4, -3, -2, 1, 2, 3, 4, 8, 16};
	}

	struct OutputClockScalingItem : MenuItem {
		Muxlicer* module;

		struct OutputClockScalingChildItem : MenuItem {
			Muxlicer* module;
			int clockOutMulDiv;
			void onAction(const event::Action& e) override {
				module->outputClockMultDiv.multDiv = clockOutMulDiv;
			}
		};

		Menu* createChildMenu() override {
			Menu* menu = new Menu;

			for (auto clockOption : getClockOptions()) {
				std::string optionString = (clockOption < 0) ? ("x 1/" + std::to_string(-clockOption)) : ("x " + std::to_string(clockOption));
				OutputClockScalingChildItem* clockItem = createMenuItem<OutputClockScalingChildItem>(optionString,
				    CHECKMARK(module->outputClockMultDiv.multDiv == clockOption));
				clockItem->clockOutMulDiv = clockOption;
				clockItem->module = module;
				menu->addChild(clockItem);
			}

			return menu;
		}
	};

	struct MainClockScalingItem : MenuItem {
		Muxlicer* module;

		struct MainClockScalingChildItem : MenuItem {
			Muxlicer* module;
			int clockOutMulDiv;
			void onAction(const event::Action& e) override {
				module->mainClockMultDiv.multDiv = clockOutMulDiv;
			}
		};

		Menu* createChildMenu() override {
			Menu* menu = new Menu;

			for (auto clockOption : getClockOptions()) {
				std::string optionString = (clockOption < 0) ? ("x 1/" + std::to_string(-clockOption)) : ("x " + std::to_string(clockOption));
				MainClockScalingChildItem* clockItem = createMenuItem<MainClockScalingChildItem>(optionString,
				                                       CHECKMARK(module->mainClockMultDiv.multDiv == clockOption));
				clockItem->clockOutMulDiv = clockOption;
				clockItem->module = module;
				menu->addChild(clockItem);
			}

			return menu;
		}
	};

	struct QuadraticGatesMenuItem : MenuItem {
		Muxlicer* module;
		void onAction(const event::Action& e) override {
			module->quadraticGatesOnly ^= true;
		}
	};

	void appendContextMenu(Menu* menu) override {
		Muxlicer* module = dynamic_cast<Muxlicer*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator());
		menu->addChild(createMenuLabel<MenuLabel>("Clock Multiplication/Division"));

		MainClockScalingItem* mainClockScaleItem = createMenuItem<MainClockScalingItem>("Input clock", "▸");
		mainClockScaleItem->module = module;
		menu->addChild(mainClockScaleItem);

		OutputClockScalingItem* outputClockScaleItem = createMenuItem<OutputClockScalingItem>("Output clock", "▸");
		outputClockScaleItem->module = module;
		menu->addChild(outputClockScaleItem);

		menu->addChild(new MenuSeparator());

		OutputRangeItem* outputRangeItem = createMenuItem<OutputRangeItem>("All In Normalled Value", "▸");
		outputRangeItem->module = module;
		menu->addChild(outputRangeItem);

		QuadraticGatesMenuItem* quadraticGatesItem = createMenuItem<QuadraticGatesMenuItem>("Gate Mode: quadratic only", CHECKMARK(module->quadraticGatesOnly));
		quadraticGatesItem->module = module;
		menu->addChild(quadraticGatesItem);

		menu->addChild(new MenuSeparator());
		menu->addChild(createMenuLabel<MenuLabel>("Input/Output mode"));

		IOMenuItem* ioItem = createMenuItem<IOMenuItem>("1 input ▸ 8 outputs",
		                     CHECKMARK(module->modeCOMIO == Muxlicer::COM_1_IN_8_OUT));
		ioItem->module = module;
		ioItem->widget = this;
		menu->addChild(ioItem);

		OIMenuItem* oiItem = createMenuItem<OIMenuItem>("8 inputs ▸ 1 output",
		                     CHECKMARK(module->modeCOMIO == Muxlicer::COM_8_IN_1_OUT));
		oiItem->module = module;
		oiItem->widget = this;
		menu->addChild(oiItem);
	}

	void clearCables() {
		for (int i = Muxlicer::MUX_OUTPUTS; i <= Muxlicer::MUX_OUTPUTS_LAST; ++i) {
			APP->scene->rack->clearCablesOnPort(outputs[i]);
		}
		APP->scene->rack->clearCablesOnPort(inputs[Muxlicer::COM_INPUT]);

		for (int i = Muxlicer::MUX_INPUTS; i <= Muxlicer::MUX_INPUTS_LAST; ++i) {
			APP->scene->rack->clearCablesOnPort(inputs[i]);
		}
		APP->scene->rack->clearCablesOnPort(outputs[Muxlicer::COM_OUTPUT]);
	}

	// set ports visibility, either for 1 input -> 8 outputs or 8 inputs -> 1 output
	void updatePortVisibilityForIOMode(Muxlicer::ModeCOMIO mode) {

		bool visibleToggle = (mode == Muxlicer::COM_1_IN_8_OUT);

		for (int i = Muxlicer::MUX_OUTPUTS; i <= Muxlicer::MUX_OUTPUTS_LAST; ++i) {
			outputs[i]->visible = visibleToggle;
		}
		inputs[Muxlicer::COM_INPUT]->visible = visibleToggle;

		for (int i = Muxlicer::MUX_INPUTS; i <= Muxlicer::MUX_INPUTS_LAST; ++i) {
			inputs[i]->visible = !visibleToggle;
		}
		outputs[Muxlicer::COM_OUTPUT]->visible = !visibleToggle;
	}


};


Model* modelMuxlicer = createModel<Muxlicer, MuxlicerWidget>("Muxlicer");

