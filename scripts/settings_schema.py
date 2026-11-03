# SPDX-License-Identifier: MIT
"""Devicetree-to-settings-schema adapter for runtime Macro records."""

import re
from types import SimpleNamespace


def _default_record(context, node, slot, record_max_length, max_instructions):
    value = context.node_value
    field = f"macro slot {slot}"
    if "bindings" not in node.props:
        if "wait-ms" in node.props or "tap-ms" in node.props:
            raise ValueError(f"{field}: wait-ms and tap-ms require bindings")
        default_wait = context.config_values.get("CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS", 15)
        default_tap = context.config_values.get("CONFIG_ZMK_MACRO_DEFAULT_TAP_MS", 30)
        return context.record_default(node, [
            1, 0, 0, 0,
            default_tap & 0xFF, default_tap >> 8,
            default_wait & 0xFF, default_wait >> 8,
        ], record_max_length, field)
    if "record-default" in node.props:
        raise ValueError(f"{field}: bindings and record-default cannot both be specified")

    bindings = value(node, "bindings")
    if not isinstance(bindings, list):
        raise ValueError(f"{field}: bindings must be a ZMK Behavior phandle-array")
    wait_ms = value(node, "wait-ms", context.config_values.get("CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS", 15))
    tap_ms = value(node, "tap-ms", context.config_values.get("CONFIG_ZMK_MACRO_DEFAULT_TAP_MS", 30))
    if type(wait_ms) is not int or not 0 <= wait_ms <= 30000:
        raise ValueError(f"{field}: wait-ms must be in 0..30000")
    if type(tap_ms) is not int or not 0 <= tap_ms <= 30000:
        raise ValueError(f"{field}: tap-ms must be in 0..30000")

    mode = "tap"
    instructions = []
    pause_seen = False

    def append_instruction(opcode, payload):
        if len(instructions) >= max_instructions:
            raise ValueError(
                f"{field}: bindings expand to more than {max_instructions} macro instructions"
            )
        instructions.append(bytes((opcode, len(payload) & 0xFF, len(payload) >> 8)) + payload)

    for binding_index, binding in enumerate(bindings):
        behavior = getattr(binding, "controller", None)
        params = getattr(binding, "data", None)
        if behavior is None or not isinstance(params, dict):
            raise ValueError(f"{field}: bindings[{binding_index}] must reference a valid Behavior")
        compat = set(behavior.compats)
        param1 = params.get("param1", 0)
        param2 = params.get("param2", 0)
        if any(type(param) is not int or not 0 <= param <= 0xFFFFFFFF
               for param in (param1, param2)):
            raise ValueError(f"{field}: bindings[{binding_index}] parameters must fit u32")

        if "zmk,macro-control-mode-tap" in compat:
            mode = "tap"
            continue
        if "zmk,macro-control-mode-press" in compat:
            mode = "press"
            continue
        if "zmk,macro-control-mode-release" in compat:
            mode = "release"
            continue
        if "zmk,macro-control-tap-time" in compat:
            if param1 > 30000:
                raise ValueError(f"{field}: macro_tap_time exceeds 30000 ms")
            append_instruction(7, param1.to_bytes(2, "little"))
            continue
        if "zmk,macro-control-wait-time" in compat:
            if param1 > 30000:
                raise ValueError(f"{field}: macro_wait_time exceeds 30000 ms")
            append_instruction(8, param1.to_bytes(2, "little"))
            continue
        if any("macro-param-" in item for item in compat):
            raise ValueError(f"{field}: Parameterized Macros are not supported")
        if "zmk,macro-pause-for-release" in compat:
            if pause_seen:
                raise ValueError(f"{field}: only one macro_pause_for_release is supported")
            pause_seen = True
            append_instruction(6, b"")
            continue

        identity = context.stable_behavior_id(behavior, field)
        action_opcode = {"tap": 1, "press": 2, "release": 3}[mode]
        payload = (
            identity.to_bytes(2, "little")
            + param1.to_bytes(4, "little")
            + param2.to_bytes(4, "little")
        )
        append_instruction(action_opcode, payload)

    payload = b"".join(instructions)
    if len(payload) + 8 > record_max_length:
        raise ValueError(f"{field}: encoded bindings exceed record-max-length")
    return [
        1, len(instructions), len(payload) & 0xFF, len(payload) >> 8,
        tap_ms & 0xFF, tap_ms >> 8, wait_ms & 0xFF, wait_ms >> 8,
    ] + list(payload)


def contribute(context):
    nodes = context.nodes
    value = context.node_value
    managers = [n for n in nodes if n.status == "okay" and "sekigon,dynamic-macro-manager" in set(n.compats)]
    behaviors = [n for n in nodes if n.status == "okay" and "sekigon,dynamic-behavior-macro" in set(n.compats)]
    maximum = context.config_values.get("CONFIG_ZMK_DYNAMIC_MACRO_MAX_SLOTS", len(behaviors))
    if type(maximum) is not int or maximum < len(behaviors):
        raise ValueError(
            "macro dispatcher count exceeds "
            f"CONFIG_ZMK_DYNAMIC_MACRO_MAX_SLOTS={maximum}"
        )
    if maximum == 0:
        return []

    max_instructions = context.config_values.get(
        "CONFIG_ZMK_DYNAMIC_MACRO_MAX_INSTRUCTIONS", 32
    )
    if type(max_instructions) is not int or not 1 <= max_instructions <= 32:
        raise ValueError("CONFIG_ZMK_DYNAMIC_MACRO_MAX_INSTRUCTIONS must be in 1..32")
    if len(managers) != 1:
        raise ValueError("runtime macro behaviors need exactly one sekigon,dynamic-macro-manager")
    default_max_length = value(managers[0], "record-max-length", 256)
    if default_max_length is not None and (
        type(default_max_length) is not int or not 8 <= default_max_length <= 540
    ):
        raise ValueError("sekigon,dynamic-macro-manager record-max-length must be in 8..540")

    groups = []
    settings_device = behaviors[0] if behaviors else managers[0]
    default_target_role = value(settings_device, "target-role", context.default_target_role())
    layout_setting = SimpleNamespace(
        status="okay",
        compats={"sekigon,dynamic-setting"},
        props={
            "setting-name": SimpleNamespace(val="os-keyboard-layout"),
            "label": SimpleNamespace(val="OS キーボード配列"),
            "value-type": SimpleNamespace(val="enum"),
            "default-enum": SimpleNamespace(val="us"),
            "enum-values": SimpleNamespace(val=["us", "japanese"]),
            "widget": SimpleNamespace(val="select"),
            "help": SimpleNamespace(
                val="Text入力の記号を、OS側で設定されているUSまたは日本語(JIS)配列に合わせます。"
            ),
            "on-change": SimpleNamespace(val="dynamic_macro_text_layout_changed"),
        },
        children={},
    )
    layout_group = context.make_group({
        "module-name": "macro", "group-name": "options", "instance-name": "text",
        "label": "Macro Text", "scope": "shared", "format-version": 1,
        "target-role": value(settings_device, "target-role", "keyboard-right"),
        "actual-device": settings_device,
    })
    layout_group.children = {"os_keyboard_layout": layout_setting}
    groups.append(layout_group)
    for slot in range(maximum):
        behavior = behaviors[slot] if slot < len(behaviors) else SimpleNamespace(
            status="okay", compats={"sekigon,dynamic-behavior-macro"},
            props={"target-role": SimpleNamespace(val=default_target_role)},
            labels=[f"macro_slot{slot}"], name=f"macro_slot{slot}",
        )
        labels = getattr(behavior, "labels", [])
        if not labels or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", labels[0]):
            raise ValueError("runtime macro behavior needs a devicetree label")
        max_length = value(behavior, "record-max-length", default_max_length)
        if type(max_length) is not int or not 8 <= max_length <= 540:
            raise ValueError(
                f"macro slot {slot}: record-max-length must be 8..540 on the behavior or manager"
            )
        record = _default_record(context, behavior, slot, max_length, max_instructions)
        if len(record) < 8:
            raise ValueError(f"macro slot {slot}: record-default must contain the 8-byte header")
        if record[0] != 1:
            raise ValueError(f"macro slot {slot}: record-default version must be 1")
        if int.from_bytes(record[2:4], "little") != len(record) - 8:
            raise ValueError(f"macro slot {slot}: record payload length does not match header")
        if any(v > 30000 for v in (int.from_bytes(record[4:6], "little"),
                                    int.from_bytes(record[6:8], "little"))):
            raise ValueError(f"macro slot {slot}: default tap-ms and wait-ms must be in 0..30000")
        if record[1] > max_instructions:
            raise ValueError(
                f"macro slot {slot}: record has {record[1]} instructions, limit is {max_instructions}"
            )
        groups.append(context.make_group({
            "module-name": "macro", "group-name": "slot", "instance-name": f"slot{slot}",
            "label": f"Macro {slot + 1}", "scope": "shared", "format-version": 1,
            "slot-index": slot,
            "target-role": value(behavior, "target-role", default_target_role),
            "actual-device": behavior if slot < len(behaviors) else None,
            "record-on-change-context-free": slot >= len(behaviors),
            "record-type": "macro-v1", "record-max-length": max_length,
            "record-default": record, "record-on-change": "dynamic_macro_record_changed",
        }))
    return groups
