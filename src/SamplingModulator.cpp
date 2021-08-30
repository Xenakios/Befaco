#include "plugin.hpp"


struct SamplingModulator : Module {

	const static int numSteps = 8;

	enum ParamIds {
		RATE_PARAM,
		FINE_PARAM,
		INT_EXT_PARAM,
		ENUMS(STEP_PARAM, numSteps),
		NUM_PARAMS
	};
	enum InputIds {
		SYNC_INPUT,
		VOCT_INPUT,
		HOLD_INPUT,
		IN_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		CLOCK_OUTPUT,
		TRIGG_OUTPUT,
		OUT_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		ENUMS(STEP_LIGHT, numSteps),
		NUM_LIGHTS
	};

	enum StepState {
		STATE_RESET,
		STATE_OFF,
		STATE_ON
	};

	enum ClockMode {
		CLOCK_EXTERNAL,
		CLOCK_INTERNAL
	};

	struct ClockTypeParam : ParamQuantity {
		std::string getDisplayValueString() override {
			if (module != nullptr && paramId == INT_EXT_PARAM) {
				return (module->params[INT_EXT_PARAM].getValue() == CLOCK_EXTERNAL) ? "External" : "Internal";
			}
			else {
				return "";
			}
		}
	};

	struct StepTypeParam : ParamQuantity {
		std::string getDisplayValueString() override {
			if (module != nullptr && STEP_PARAM <= paramId && STEP_PARAM < STEP_PARAM_LAST) {
				StepState stepState = (StepState) module->params[paramId].getValue();

				if (stepState == STATE_RESET) {
					return "Reset";
				}
				else if (stepState == STATE_OFF) {
					return "Off";
				}
				else {
					return "On";
				}
			}
			else {
				return "";
			}
		}
	};

	int numEffectiveSteps = numSteps;
	int currentStep = 0;
	StepState stepStates[numSteps];

	dsp::PulseGenerator triggerGenerator;
	dsp::SchmittTrigger holdDetector;
	dsp::SchmittTrigger clock;
	dsp::MinBlepGenerator<16, 32> squareMinBlep;
	dsp::MinBlepGenerator<16, 32> triggMinBlep;
	dsp::MinBlepGenerator<16, 32> holdMinBlep;
	bool removeDC = true;

	float stepPhase = 0.f;
	float heldValue = 0.f;
	/** Whether we are past the pulse width already */
	bool halfPhase = false;

	SamplingModulator() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(RATE_PARAM, 0.0f, 1.f, 0.f, "Rate");
		configParam(FINE_PARAM, 0.f, 1.f, 0.f, "Fine tune");
		configParam<ClockTypeParam>(INT_EXT_PARAM, 0.f, 1.f, CLOCK_INTERNAL, "Clock");

		for (int i = 0; i < numSteps; i++) {
			configParam<StepTypeParam>(STEP_PARAM + i, 0.f, 2.f, STATE_ON, "Step " + std::to_string(i + 1));
		}
	}

	void process(const ProcessArgs& args) override {
		bool advanceStep = false;

		if (params[INT_EXT_PARAM].getValue() == CLOCK_EXTERNAL) {
			// if external mode, the SYNC/EXT. CLOCK input acts as a clock
			advanceStep = clock.process(rescale(inputs[SYNC_INPUT].getVoltage(), 0.1f, 2.f, 0.f, 1.f));
		}
		else {
			// if internal mode, the SYNC/EXT. CLOCK input acts as oscillator sync, resetting the phase
			if (clock.process(rescale(inputs[SYNC_INPUT].getVoltage(), 0.1f, 2.f, 0.f, 1.f))) {
				advanceStep = true;
				stepPhase = 0.f;
				halfPhase = false;
			}
		}

		if (holdDetector.process(rescale(inputs[HOLD_INPUT].getVoltage(), 0.1f, 2.f, 0.f, 1.f))) {
			float oldHeldValue = heldValue;
			heldValue = inputs[IN_INPUT].getVoltage();
			holdMinBlep.insertDiscontinuity(0, heldValue - oldHeldValue);
		}

		for (int i = 0; i < numSteps; i++) {
			stepStates[i] = (StepState) params[STEP_PARAM + i].getValue();
		}
		int numActiveSteps = 0;
		numEffectiveSteps = 8;
		for (int i = 0; i < numSteps; i++) {
			numActiveSteps += (stepStates[i] == STATE_ON);
			if (stepStates[i] == STATE_RESET) {
				numEffectiveSteps = i;
				break;
			}
		}

		const float pitch = 16.f * params[RATE_PARAM].getValue() + params[FINE_PARAM].getValue() + inputs[VOCT_INPUT].getVoltage();
		const float minDialFrequency = 1.0f;
		const float frequency = minDialFrequency * simd::pow(2.f, pitch);

		float oldPhase = stepPhase;
		float deltaPhase = clamp(args.sampleTime * frequency, 1e-6f, 0.5f);
		stepPhase += deltaPhase;

		if (!halfPhase && stepPhase >= 0.5) {
			float crossing  = -(stepPhase - 0.5) / deltaPhase;
			squareMinBlep.insertDiscontinuity(crossing, -2.f);
			if (stepStates[currentStep] == STATE_ON) {
				triggMinBlep.insertDiscontinuity(crossing, -2.f);
			}

			halfPhase = true;
		}

		if (stepPhase >= 1.0f) {
			stepPhase -= 1.0f;
			float crossing = -stepPhase / deltaPhase;
			squareMinBlep.insertDiscontinuity(crossing, +2.f);

			halfPhase = false;

			if (params[INT_EXT_PARAM].getValue() == CLOCK_INTERNAL) {
				advanceStep = true;
			}
		}

		if (advanceStep) {
			currentStep = (currentStep + 1) % std::max(1, numEffectiveSteps);

			if (stepStates[currentStep] == STATE_ON) {
				const float crossing = -(oldPhase + deltaPhase - 1.0) / deltaPhase;
				triggMinBlep.insertDiscontinuity(crossing, +2.f);
				triggerGenerator.trigger();
				if (!holdDetector.isHigh()) {
					float oldHeldValue = heldValue;
					heldValue = inputs[IN_INPUT].getVoltage();
					holdMinBlep.insertDiscontinuity(crossing, heldValue - oldHeldValue);
				}
			}
		}

		float output = heldValue + holdMinBlep.process();
		outputs[OUT_OUTPUT].setVoltage(output);

		float square = (stepPhase < 0.5) ? 2.f : 0.f;
		square += squareMinBlep.process();

		float trigger = (stepPhase < 0.5 && stepStates[currentStep] == STATE_ON) ? 2.f : 0.f;
		trigger += triggMinBlep.process();

		if (removeDC) {
			trigger -= 1.0f;
			square -= 1.0f;
			if (numEffectiveSteps > 0) {
				trigger += (float)(numEffectiveSteps - numActiveSteps) / (numEffectiveSteps);
			}
		}

		outputs[CLOCK_OUTPUT].setVoltage(5.f * square);
		if (params[INT_EXT_PARAM].getValue() == CLOCK_INTERNAL) {
			outputs[TRIGG_OUTPUT].setVoltage(5.f * trigger);
		}
		else {			
			outputs[TRIGG_OUTPUT].setVoltage(10.f * triggerGenerator.process(args.sampleTime));
		}

		for (int i = 0; i < numSteps; i++) {
			lights[STEP_LIGHT + i].setBrightness(currentStep == i);
		}
	}
};


struct SamplingModulatorWidget : ModuleWidget {
	SamplingModulatorWidget(SamplingModulator* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/SamplingModulator.svg")));

		addChild(createWidget<Knurlie>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<Knurlie>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<Knurlie>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<Knurlie>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Davies1900hWhiteKnob>(mm2px(Vec(9.72, 38.019)), module, SamplingModulator::RATE_PARAM));
		addParam(createParamCentered<Davies1900hWhiteKnob>(mm2px(Vec(30.921, 38.019)), module, SamplingModulator::FINE_PARAM));
		addParam(createParamCentered<BefacoSwitch>(mm2px(Vec(20.313, 52.642)), module, SamplingModulator::INT_EXT_PARAM));
		addParam(createParamCentered<BefacoSwitch>(mm2px(Vec(8.319, 57.761)), module, SamplingModulator::STEP_PARAM + 0));
		addParam(createParamCentered<BefacoSwitch>(mm2px(Vec(8.319, 71.758)), module, SamplingModulator::STEP_PARAM + 1));
		addParam(createParamCentered<BefacoSwitch>(mm2px(Vec(8.319, 85.769)), module, SamplingModulator::STEP_PARAM + 2));
		addParam(createParamCentered<BefacoSwitch>(mm2px(Vec(8.319, 99.804)), module, SamplingModulator::STEP_PARAM + 3));
		addParam(createParamCentered<BefacoSwitch>(mm2px(Vec(32.326, 57.761)), module, SamplingModulator::STEP_PARAM + 4));
		addParam(createParamCentered<BefacoSwitch>(mm2px(Vec(32.326, 71.758)), module, SamplingModulator::STEP_PARAM + 5));
		addParam(createParamCentered<BefacoSwitch>(mm2px(Vec(32.326, 85.769)), module, SamplingModulator::STEP_PARAM + 6));
		addParam(createParamCentered<BefacoSwitch>(mm2px(Vec(32.326, 99.804)), module, SamplingModulator::STEP_PARAM + 7));

		addInput(createInputCentered<BefacoInputPort>(mm2px(Vec(7.426, 16.737)), module, SamplingModulator::SYNC_INPUT));
		addInput(createInputCentered<BefacoInputPort>(mm2px(Vec(20.313, 28.175)), module, SamplingModulator::VOCT_INPUT));
		addInput(createInputCentered<BefacoInputPort>(mm2px(Vec(20.342, 111.762)), module, SamplingModulator::HOLD_INPUT));
		addInput(createInputCentered<BefacoInputPort>(mm2px(Vec(7.426, 114.484)), module, SamplingModulator::IN_INPUT));

		addOutput(createOutputCentered<BefacoOutputPort>(mm2px(Vec(20.313, 14.417)), module, SamplingModulator::CLOCK_OUTPUT));
		addOutput(createOutputCentered<BefacoOutputPort>(mm2px(Vec(33.224, 16.737)), module, SamplingModulator::TRIGG_OUTPUT));
		addOutput(createOutputCentered<BefacoOutputPort>(mm2px(Vec(33.224, 114.484)), module, SamplingModulator::OUT_OUTPUT));

		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(16.921, 62.208)), module, SamplingModulator::STEP_LIGHT + 0));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(16.921, 73.011)), module, SamplingModulator::STEP_LIGHT + 1));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(16.921, 83.814)), module, SamplingModulator::STEP_LIGHT + 2));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(16.921, 94.617)), module, SamplingModulator::STEP_LIGHT + 3));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(23.722, 62.208)), module, SamplingModulator::STEP_LIGHT + 4));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(23.722, 73.011)), module, SamplingModulator::STEP_LIGHT + 5));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(23.722, 83.814)), module, SamplingModulator::STEP_LIGHT + 6));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(23.722, 94.617)), module, SamplingModulator::STEP_LIGHT + 7));
	}

	struct DCMenuItem : MenuItem {
		SamplingModulator* module;
		void onAction(const event::Action& e) override {
			module->removeDC ^= true;
		}
	};

	void appendContextMenu(Menu* menu) override {
		SamplingModulator* module = dynamic_cast<SamplingModulator*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator());

		DCMenuItem* dcItem = createMenuItem<DCMenuItem>("Remove DC Offset", CHECKMARK(module->removeDC));
		dcItem->module = module;
		menu->addChild(dcItem);
	}
};


Model* modelSamplingModulator = createModel<SamplingModulator, SamplingModulatorWidget>("SamplingModulator");