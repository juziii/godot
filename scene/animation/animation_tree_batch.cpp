#include "animation_batch_processor.h"

#ifndef _3D_DISABLED
#include "core/object/class_db.h"
#include "scene/animation/animation_tree.h"
#include "scene/animation/animation_node_state_machine.h"

bool AnimationTree::_prepare_batch_graph() {
    const bool changed = properties_dirty || validation_dirty || !batch_graph_checked;
    _update_properties();
    if (validation_dirty) { _update_connections(); validation_dirty = false; }
    if (root_animation_node.is_null()) { batch_fallback_reason = "Empty animation graph"; return false; }
    if (!changed) { batch_fallback_reason = batch_graph_reason; return batch_graph_reason.is_empty(); }
    batch_graph_checked = false;
    for (const KeyValue<StringName, AnimationNodeInstance> &kv : instance_map) {
        const Ref<AnimationNode> &node = kv.value.resource;
        if (node->get_script_instance()) { batch_fallback_reason = "Script animation node"; return false; }
        const StringName type = node->get_class_name();
        if (type != SNAME("AnimationNodeBlendTree") && type != SNAME("AnimationNodeStateMachine") &&
            type != SNAME("AnimationNodeAnimation") && type != SNAME("AnimationNodeTimeScale") &&
            type != SNAME("AnimationNodeTimeSeek") && type != SNAME("AnimationNodeBlend2") &&
            type != SNAME("AnimationNodeAdd2") && type != SNAME("AnimationNodeOneShot") &&
            type != SNAME("AnimationNodeBlendSpace1D") && type != SNAME("AnimationNodeBlendSpace2D") &&
            type != SNAME("AnimationNodeOutput") && type != SNAME("AnimationNodeStartState") && type != SNAME("AnimationNodeEndState")) {
            batch_fallback_reason = "Unsupported animation node: " + String(type); return false;
        }
        AnimationNodeStateMachine *machine = Object::cast_to<AnimationNodeStateMachine>(node.ptr());
        if (machine) {
            for (int i = 0; i < machine->get_transition_count(); ++i) {
                if (!machine->get_transition(i)->get_advance_expression().is_empty()) {
                    batch_fallback_reason = "Scene-dependent transition expression"; return false;
                }
            }
        }
        Variant *const *observer = kv.value.property_ptrs.getptr(SNAME("observer"));
        if (observer && (*observer)->get_type() == Variant::OBJECT && static_cast<Object *>(**observer) != nullptr) {
            batch_fallback_reason = "Animation observer requires synchronous evaluation"; return false;
        }
    }
    batch_graph_checked = true;
    batch_graph_reason = String();
    return true;
}
#endif
