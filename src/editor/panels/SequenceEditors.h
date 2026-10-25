#pragma once
//
// SequenceEditors.h -- Properties panel widgets for the effect value types.
// Each returns true when the value was changed this frame. The editors
// (popup content) also report `discrete`: a click that should be one undo
// step by itself (adding / removing a point, a preset) rather than part of
// a drag.
//
#include "scene/Scene.h"
#include "scene/Value.h"

namespace rift {

// Min and max side by side.
bool numberRangeField(const char* id, NumberRange& range, float speed, float lo, float hi);

// A one-line preview (curve and values); true when clicked (open the editor).
bool numberSequencePreview(const char* id, const NumberSequence& seq, float lo, float hi, bool bounded);
// The full editor: a curve with draggable points, the points as numbers, presets.
bool numberSequenceEditor(NumberSequence& seq, float lo, float hi, bool bounded, bool& discrete);

bool colorSequencePreview(const char* id, const ColorSequence& seq);
// A gradient bar with colour stops, a colour picker for the selected stop, presets.
bool colorSequenceEditor(ColorSequence& seq, bool& discrete);

// A combo of the scene's Attachments (or parts, for Motor6D.Part0 / Part1),
// those near `near` first; also accepts one dragged from the Explorer.
bool attachmentPicker(const char* id, const Scene& scene, InstanceRef& ref, uint64_t near, bool parts = false);

} // namespace rift
