/*
 * JP-8000 UI for Schwung
 *
 * Thin wrapper over the shared sound generator UI. In a Signal Chain slot the
 * shadow UI renders the plugin's ui_hierarchy directly; this file only serves
 * the standalone host. Patch/performance browsing and every edit go through
 * the plugin's get_param/set_param (see dsp/jp8000_plugin.cpp).
 */

import { createSoundGeneratorUI } from '/data/UserData/schwung/shared/sound_generator_ui.mjs';

const ui = createSoundGeneratorUI({
    moduleName: 'JP-8000',
    showPolyphony: false,
    showOctave: false,
});

globalThis.init = ui.init;
globalThis.tick = ui.tick;
globalThis.onMidiMessageInternal = ui.onMidiMessageInternal;
globalThis.onMidiMessageExternal = ui.onMidiMessageExternal;
