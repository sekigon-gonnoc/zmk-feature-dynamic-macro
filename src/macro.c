/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT sekigon_dynamic_behavior_macro

#include <errno.h>
#include <string.h>

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/dynamic_settings/store.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

#include "settings_schema_generated.h"
#include <zmk/dynamic_macro/macro.h>

LOG_MODULE_REGISTER(dynamic_macro, CONFIG_ZMK_LOG_LEVEL);

#define MACRO_SLOT_COUNT CONFIG_ZMK_DYNAMIC_MACRO_MAX_SLOTS
#define MACRO_DISPATCHER_COUNT DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT)
#define MACRO_MANAGER_COUNT DT_NUM_INST_STATUS_OKAY(sekigon_dynamic_macro_manager)
#define MACRO_RECORD_VERSION 1
#define MACRO_RECORD_HEADER_SIZE 8
#define MACRO_INSTRUCTION_HEADER_SIZE 3
#define MACRO_HELD_MAX DYNAMIC_MACRO_MAX_INSTRUCTIONS
#define MACRO_QUEUE_RETRY_MS 5

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
#define MACRO_EFFECTIVE_INTERVAL_MS ((CONFIG_ZMK_SPLIT_BLE_PREF_INT * 5 + 3) / 4)
#elif IS_ENABLED(CONFIG_BT)
#define MACRO_EFFECTIVE_INTERVAL_MS ((CONFIG_BT_PERIPHERAL_PREF_MIN_INT * 5 + 3) / 4)
#else
#define MACRO_EFFECTIVE_INTERVAL_MS CONFIG_ZMK_DYNAMIC_MACRO_MINIMUM_INTERVAL_MS
#endif

BUILD_ASSERT(MACRO_DISPATCHER_COUNT > 0, "at least one macro behavior must be enabled");
BUILD_ASSERT(MACRO_DISPATCHER_COUNT <= MACRO_SLOT_COUNT,
             "increase CONFIG_ZMK_DYNAMIC_MACRO_MAX_SLOTS for this many behaviors");
BUILD_ASSERT(MACRO_MANAGER_COUNT == 1,
             "exactly one sekigon,dynamic-macro-manager node is required");
BUILD_ASSERT(MACRO_EFFECTIVE_INTERVAL_MS > 0, "macro event interval must be positive");

struct runtime_instruction {
    uint32_t value1;
    uint32_t value2;
    uint16_t behavior_id;
    uint8_t opcode;
    uint8_t length;
} __packed;

struct macro_slot_timing {
    uint16_t tap_ms;
    uint16_t wait_ms;
};

enum macro_text_layout {
    MACRO_TEXT_LAYOUT_US,
    MACRO_TEXT_LAYOUT_JAPANESE,
};

struct macro_slot {
    const struct device *device;
    const struct dynamic_setting_group_descriptor *settings;
};

struct held_binding {
    /* Original identity is used to match a later Macro RELEASE instruction. */
    struct zmk_behavior_binding binding;
    /* The effective binding may have Macro-owned modifiers stripped. */
    struct zmk_behavior_binding effective_binding;
    uint8_t modifiers;
    bool suppressed;
};

enum runner_state { RUNNER_IDLE, RUNNER_RUNNING, RUNNER_WAITING, RUNNER_CLEANUP };
enum runner_wait {
    RUNNER_WAIT_NONE,
    RUNNER_WAIT_TAP_RELEASE,
    RUNNER_WAIT_TEXT_PRESS,
    RUNNER_WAIT_TEXT_RELEASE,
    RUNNER_WAIT_PAUSE_FOR_RELEASE
};

struct macro_runner {
    struct k_work_delayable work;
    enum runner_state state;
    enum runner_wait wait;
    struct zmk_behavior_binding_event event;
    const uint8_t *record;
    struct held_binding held[MACRO_HELD_MAX];
    struct zmk_behavior_binding pending_binding;
    struct runtime_instruction instruction;
    uint32_t deadline;
    uint16_t record_length;
    uint16_t record_offset;
    uint16_t slot;
    uint16_t tap_ms;
    uint16_t wait_ms;
    uint16_t cursor;
    uint16_t end;
    uint8_t held_count;
    uint8_t text_index;
    uint8_t text_layout;
    bool instruction_loaded;
    bool trigger_released;
    bool cancel_requested;
    int result;
};

static struct macro_slot macro_slots[MACRO_SLOT_COUNT];
static uint16_t macro_key_press_id = UINT16_MAX;
static bool macro_settings_ready;
static enum macro_text_layout macro_text_layout = MACRO_TEXT_LAYOUT_US;
static struct macro_runner runner;
K_MUTEX_DEFINE(macro_mutex);

dynamic_setting_record_on_change_fn dynamic_macro_record_changed;

static bool macro_record_group(const struct dynamic_setting_group_descriptor *group) {
    return group && (group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) &&
           group->record_on_change == dynamic_macro_record_changed;
}

static uint16_t behavior_identity(const char *name) {
    uint32_t hash = 2166136261u;
    for (const uint8_t *p = (const uint8_t *)name; *p; p++) {
        hash = (hash ^ *p) * 16777619u;
    }
    hash ^= hash >> 16;
    return (uint16_t)hash;
}

static const struct device *resolve_behavior(uint16_t identity, bool require_ready) {
    const struct device *match = NULL;
    STRUCT_SECTION_FOREACH(zmk_behavior_ref, ref) {
        if (!ref->device) {
            continue;
        }
        bool identity_matches = behavior_identity(ref->device->name) == identity;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
        /* Studio serializes the Behavior's metadata display name. Preserve the
         * device name identity too, since schema defaults use the DT name. */
        if (ref->metadata.display_name &&
            behavior_identity(ref->metadata.display_name) == identity) {
            identity_matches = true;
        }
#endif
        if (!identity_matches) {
            continue;
        }
        if (match && match != ref->device) {
            return NULL;
        }
        match = ref->device;
    }
    if (require_ready && (!match || !device_is_ready(match))) {
        return NULL;
    }
    return match;
}

static bool is_macro_device(const struct device *device) {
    for (uint16_t i = 0; i < MACRO_SLOT_COUNT; i++) {
        if (macro_slots[i].device == device) {
            return true;
        }
    }
    return false;
}

static int group_record(const struct dynamic_setting_group_descriptor *group,
                        const struct dynamic_setting_group_descriptor *override_group,
                        const uint8_t *override_record, size_t override_length,
                        const uint8_t **record, size_t *length) {
    if (group == override_group && override_record) {
        *record = override_record;
        *length = override_length;
        return 0;
    }
    /* Record callbacks run under the Settings operation lock. Avoid re-entering
     * it through dynamic_settings_record_read() and inspect the stable state. */
    if (!group->state) {
        return -ENOTSUP;
    }
    bool use_default = group->state->record_value_state == DYNAMIC_SETTING_VALUE_EMPTY;
    const uint8_t *effective_record =
        use_default ? group->default_record : group->state->record_data;
    size_t record_length = use_default ? group->default_record_length : group->state->record_length;
    if (record_length > group->record_max_length || (!effective_record && record_length)) {
        return -ENOSPC;
    }
    *record = effective_record;
    *length = record_length;
    return 0;
}

static uint8_t explicit_modifier_for_behavior(uint16_t behavior_id, uint32_t param1) {
    if (behavior_id != macro_key_press_id) {
        return 0;
    }
    uint8_t modifiers = SELECT_MODS(param1);
    uint8_t page = ZMK_HID_USAGE_PAGE(param1);
    uint16_t keycode = ZMK_HID_USAGE_ID(param1);
    if (page == HID_USAGE_KEY && is_mod(page, keycode)) {
        modifiers |= BIT(keycode - HID_USAGE_KEY_KEYBOARD_LEFTCONTROL);
    }
    return modifiers;
}

static int decode_record(const uint8_t *record, size_t length, bool validate_bindings,
                         uint16_t *instruction_count, uint16_t *text_bytes, uint32_t *duration_ms,
                         struct macro_slot_timing *timing, struct runtime_instruction *output,
                         uint8_t *text_destination, size_t text_capacity, size_t *text_written) {
    if (!record || length < MACRO_RECORD_HEADER_SIZE || length > DYNAMIC_MACRO_MAX_RECORD_LENGTH ||
        record[0] != MACRO_RECORD_VERSION || record[1] > DYNAMIC_MACRO_MAX_INSTRUCTIONS ||
        sys_get_le16(record + 2) != length - MACRO_RECORD_HEADER_SIZE) {
        return -EINVAL;
    }
    uint16_t count = record[1];
    size_t offset = MACRO_RECORD_HEADER_SIZE;
    uint16_t tap_ms = sys_get_le16(record + 4);
    uint16_t wait_ms = sys_get_le16(record + 6);
    if (tap_ms > DYNAMIC_MACRO_MAX_DURATION_MS || wait_ms > DYNAMIC_MACRO_MAX_DURATION_MS) {
        return -ERANGE;
    }
    uint16_t bytes = 0;
    uint32_t duration = 0;
    struct zmk_behavior_binding held[MACRO_HELD_MAX];
    uint8_t held_count = 0;
    bool pause_seen = false;
    size_t text_cursor = 0;

    for (uint16_t i = 0; i < count; i++) {
        if (offset + MACRO_INSTRUCTION_HEADER_SIZE > length) {
            return -EINVAL;
        }
        uint8_t opcode = record[offset];
        uint16_t payload_length = sys_get_le16(record + offset + 1);
        offset += MACRO_INSTRUCTION_HEADER_SIZE;
        if (offset + payload_length > length) {
            return -EINVAL;
        }
        const uint8_t *payload = record + offset;
        struct runtime_instruction instruction = {.opcode = opcode};
        switch (opcode) {
        case DYNAMIC_MACRO_ACTION_BEHAVIOR_TAP:
        case DYNAMIC_MACRO_ACTION_BEHAVIOR_PRESS:
        case DYNAMIC_MACRO_ACTION_BEHAVIOR_RELEASE: {
            if (payload_length != 10) {
                return -EINVAL;
            }
            uint16_t identity = sys_get_le16(payload);
            uint32_t param1 = sys_get_le32(payload + 2);
            uint32_t param2 = sys_get_le32(payload + 6);
            const struct device *device = resolve_behavior(identity, validate_bindings);
            if (!device || is_macro_device(device)) {
                return -ENOENT;
            }
            uint16_t local_id =
                validate_bindings ? zmk_behavior_get_local_id(device->name) : identity;
            if (validate_bindings && local_id == UINT16_MAX) {
                return -ENOENT;
            }
            if (validate_bindings) {
                struct zmk_behavior_binding binding = {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
                    .local_id = local_id,
#endif
                    .behavior_dev = device->name,
                    .param1 = param1,
                    .param2 = param2,
                };
                if (zmk_behavior_validate_binding(&binding)) {
                    return -EINVAL;
                }
                for (uint8_t j = 0; j < MACRO_SLOT_COUNT; j++) {
                    if (macro_slots[j].device == device) {
                        return -EINVAL;
                    }
                }
            }
            instruction.behavior_id = local_id;
            instruction.value1 = param1;
            instruction.value2 = param2;
            if (opcode == DYNAMIC_MACRO_ACTION_BEHAVIOR_TAP) {
                duration += MAX(tap_ms, MACRO_EFFECTIVE_INTERVAL_MS);
            } else {
                struct zmk_behavior_binding binding = {
                    .behavior_dev = device->name,
                    .param1 = param1,
                    .param2 = param2,
                };
                int match = -1;
                for (uint8_t j = 0; j < held_count; j++) {
                    if (strcmp(held[j].behavior_dev, binding.behavior_dev) == 0 &&
                        held[j].param1 == param1 && held[j].param2 == param2) {
                        match = j;
                        break;
                    }
                }
                if (opcode == DYNAMIC_MACRO_ACTION_BEHAVIOR_PRESS) {
                    if (match >= 0 || held_count >= MACRO_HELD_MAX) {
                        return -EINVAL;
                    }
                    held[held_count++] = binding;
                } else {
                    if (match < 0) {
                        return -EINVAL;
                    }
                    held_count--;
                    memmove(&held[match], &held[match + 1], (held_count - match) * sizeof(held[0]));
                }
            }
            duration += MAX(wait_ms, MACRO_EFFECTIVE_INTERVAL_MS);
            instruction.length = payload_length;
            break;
        }
        case DYNAMIC_MACRO_ACTION_WAIT:
            if (payload_length != 4) {
                return -EINVAL;
            }
            instruction.value1 = sys_get_le32(payload);
            if (instruction.value1 > DYNAMIC_MACRO_MAX_DURATION_MS) {
                return -ERANGE;
            }
            duration += instruction.value1;
            break;
        case DYNAMIC_MACRO_ACTION_PAUSE_FOR_RELEASE:
            if (payload_length != 0 || pause_seen) {
                return -EINVAL;
            }
            pause_seen = true;
            break;
        case DYNAMIC_MACRO_ACTION_SET_TAP_MS:
        case DYNAMIC_MACRO_ACTION_SET_WAIT_MS: {
            if (payload_length != 2) {
                return -EINVAL;
            }
            uint16_t value = sys_get_le16(payload);
            if (value > DYNAMIC_MACRO_MAX_DURATION_MS) {
                return -ERANGE;
            }
            if (opcode == DYNAMIC_MACRO_ACTION_SET_TAP_MS) {
                tap_ms = value;
            } else {
                wait_ms = value;
            }
            instruction.value1 = value;
            break;
        }
        case DYNAMIC_MACRO_ACTION_TEXT_ASCII:
            if (!payload_length || payload_length > DYNAMIC_MACRO_MAX_TEXT_BYTES) {
                return -EINVAL;
            }
            for (uint16_t j = 0; j < payload_length; j++) {
                if (payload[j] < 0x20 || payload[j] > 0x7e) {
                    return -EINVAL;
                }
            }
            if (output) {
                instruction.value1 = text_cursor;
                if (text_cursor + payload_length > text_capacity) {
                    return -ENOSPC;
                }
                memcpy(text_destination + text_cursor, payload, payload_length);
                text_cursor += payload_length;
            }
            bytes += payload_length;
            duration += payload_length * (2 * MACRO_EFFECTIVE_INTERVAL_MS);
            duration += (payload_length - 1) * MACRO_EFFECTIVE_INTERVAL_MS;
            instruction.length = payload_length;
            break;
        default:
            return -ENOTSUP;
        }
        if (output) {
            output[i] = instruction;
        }
        offset += payload_length;
        if (duration > DYNAMIC_MACRO_MAX_DURATION_MS) {
            return -ERANGE;
        }
    }
    if (offset != length) {
        return -EINVAL;
    }
    if (instruction_count) {
        *instruction_count = count;
    }
    if (text_bytes) {
        *text_bytes = bytes;
    }
    if (duration_ms) {
        *duration_ms = duration;
    }
    if (timing) {
        timing->tap_ms = sys_get_le16(record + 4);
        timing->wait_ms = sys_get_le16(record + 6);
    }
    if (text_written) {
        *text_written = text_cursor;
    }
    return 0;
}

static int decode_next_instruction(const uint8_t *record, size_t record_length, uint16_t *offset,
                                   struct runtime_instruction *instruction) {
    if (!record || !offset || !instruction ||
        *offset + MACRO_INSTRUCTION_HEADER_SIZE > record_length) {
        return -EINVAL;
    }
    size_t cursor = *offset;
    uint8_t opcode = record[cursor];
    uint16_t payload_length = sys_get_le16(record + cursor + 1);
    cursor += MACRO_INSTRUCTION_HEADER_SIZE;
    if (cursor + payload_length > record_length) {
        return -EINVAL;
    }
    const uint8_t *payload = record + cursor;
    *instruction = (struct runtime_instruction){.opcode = opcode, .length = payload_length};
    switch (opcode) {
    case DYNAMIC_MACRO_ACTION_BEHAVIOR_TAP:
    case DYNAMIC_MACRO_ACTION_BEHAVIOR_PRESS:
    case DYNAMIC_MACRO_ACTION_BEHAVIOR_RELEASE: {
        if (payload_length != 10) {
            return -EINVAL;
        }
        uint16_t identity = sys_get_le16(payload);
        const struct device *device = resolve_behavior(identity, true);
        if (!device || is_macro_device(device)) {
            return -ENOENT;
        }
        uint16_t local_id = zmk_behavior_get_local_id(device->name);
        if (local_id == UINT16_MAX) {
            return -ENOENT;
        }
        instruction->behavior_id = local_id;
        instruction->value1 = sys_get_le32(payload + 2);
        instruction->value2 = sys_get_le32(payload + 6);
        break;
    }
    case DYNAMIC_MACRO_ACTION_WAIT:
        if (payload_length != 4) {
            return -EINVAL;
        }
        instruction->value1 = sys_get_le32(payload);
        break;
    case DYNAMIC_MACRO_ACTION_PAUSE_FOR_RELEASE:
        if (payload_length) {
            return -EINVAL;
        }
        break;
    case DYNAMIC_MACRO_ACTION_SET_TAP_MS:
    case DYNAMIC_MACRO_ACTION_SET_WAIT_MS:
        if (payload_length != 2) {
            return -EINVAL;
        }
        instruction->value1 = sys_get_le16(payload);
        break;
    case DYNAMIC_MACRO_ACTION_TEXT_ASCII:
        if (!payload_length || payload_length > DYNAMIC_MACRO_MAX_TEXT_BYTES) {
            return -EINVAL;
        }
        instruction->value1 = cursor;
        break;
    default:
        return -ENOTSUP;
    }
    *offset = cursor + payload_length;
    return 0;
}

static int pin_macro_records(uint16_t active_slot, const uint8_t **active_record,
                             size_t *active_length) {
    uint16_t pinned = 0;
    for (; pinned < MACRO_SLOT_COUNT; pinned++) {
        const uint8_t *record;
        size_t length;
        int err =
            dynamic_settings_record_acquire(macro_slots[pinned].settings->id, &record, &length);
        if (err) {
            while (pinned) {
                pinned--;
                dynamic_settings_record_release(macro_slots[pinned].settings->id);
            }
            return err;
        }
        if (pinned == active_slot) {
            *active_record = record;
            *active_length = length;
        }
    }
    return 0;
}

static void unpin_macro_records(void) {
    for (uint16_t slot = 0; slot < MACRO_SLOT_COUNT; slot++) {
        dynamic_settings_record_release(macro_slots[slot].settings->id);
    }
}

static void advance_instruction(struct macro_runner *current) {
    current->cursor++;
    current->instruction_loaded = false;
}

static struct zmk_behavior_binding
instruction_binding(const struct runtime_instruction *instruction) {
    const char *name = zmk_behavior_find_behavior_name_from_local_id(instruction->behavior_id);
    struct zmk_behavior_binding binding = {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        .local_id = instruction->behavior_id,
#endif
        .behavior_dev = name,
        .param1 = instruction->value1,
        .param2 = instruction->value2,
    };
    return binding;
}

static bool binding_equal(const struct zmk_behavior_binding *a,
                          const struct zmk_behavior_binding *b) {
    return a->behavior_dev && b->behavior_dev && strcmp(a->behavior_dev, b->behavior_dev) == 0 &&
           a->param1 == b->param1 && a->param2 == b->param2;
}

static int queue_event(const struct zmk_behavior_binding *binding, bool pressed) {
    struct zmk_behavior_binding_event event = runner.event;
    event.timestamp = k_uptime_get();
    int err = zmk_behavior_queue_add(&event, *binding, pressed, MACRO_EFFECTIVE_INTERVAL_MS);
    LOG_DBG("Macro queue %s %s param1=0x%08x result=%d", binding->behavior_dev,
            pressed ? "press" : "release", binding->param1, err);
    return err;
}

static void schedule_retry(void) {
    runner.state = RUNNER_WAITING;
    k_work_reschedule(&runner.work, K_MSEC(MACRO_QUEUE_RETRY_MS));
}

static void schedule_interval(void) {
    runner.state = RUNNER_WAITING;
    k_work_reschedule(&runner.work, K_MSEC(MACRO_EFFECTIVE_INTERVAL_MS));
}

static void schedule_action_delay(uint16_t delay_ms) {
    runner.state = RUNNER_WAITING;
    k_work_reschedule(&runner.work, K_MSEC(MAX(delay_ms, MACRO_EFFECTIVE_INTERVAL_MS)));
}

static void finish_runner(void) {
    LOG_INF("Macro runner finished slot=%u result=%d", runner.slot, runner.result);
    if (runner.record) {
        unpin_macro_records();
    }
    runner.record = NULL;
    runner.record_length = 0;
    runner.instruction_loaded = false;
    runner.held_count = 0;
    runner.wait = RUNNER_WAIT_NONE;
    runner.text_index = 0;
    runner.trigger_released = false;
    runner.state = RUNNER_IDLE;
    runner.cancel_requested = false;
    runner.cursor = 0;
    runner.result = 0;
}

static void begin_cleanup(int result) {
    runner.result = result;
    runner.wait = RUNNER_WAIT_NONE;
    runner.state = RUNNER_CLEANUP;
    k_work_reschedule(&runner.work, K_NO_WAIT);
}

static void remove_held(uint8_t index) {
    runner.held_count--;
    if (index < runner.held_count) {
        memmove(&runner.held[index], &runner.held[index + 1],
                (runner.held_count - index) * sizeof(runner.held[0]));
    }
}

static int held_find(const struct zmk_behavior_binding *binding) {
    for (uint8_t i = 0; i < runner.held_count; i++) {
        if (binding_equal(&runner.held[i].binding, binding)) {
            return i;
        }
    }
    return -1;
}

static uint8_t macro_held_modifiers(void) {
    uint8_t modifiers = 0;
    for (uint8_t i = 0; i < runner.held_count; i++) {
        if (!runner.held[i].suppressed) {
            modifiers |= runner.held[i].modifiers;
        }
    }
    return modifiers;
}

static uint8_t keymap_held_modifiers(void) {
    /*
     * ZMK exposes an aggregate modifier mask, not its source. Subtract the mask
     * Macro currently owns. ZMK's per-modifier reference counts still ensure a
     * Macro release cannot clear a keymap hold of the same modifier.
     */
    return zmk_hid_get_explicit_mods() & (uint8_t)~macro_held_modifiers();
}

static int add_held(const struct zmk_behavior_binding *binding,
                    const struct zmk_behavior_binding *effective_binding, bool suppressed) {
    if (runner.held_count >= ARRAY_SIZE(runner.held)) {
        return -ENOSPC;
    }
    runner.held[runner.held_count++] = (struct held_binding){
        .binding = *binding,
        .effective_binding = *effective_binding,
        .modifiers =
            explicit_modifier_for_behavior(effective_binding->local_id, effective_binding->param1),
        .suppressed = suppressed,
    };
    return 0;
}

static bool char_to_key(uint8_t ch, enum macro_text_layout layout, uint32_t *encoded) {
    uint8_t mods = 0;
    uint8_t key = 0;
    if (layout == MACRO_TEXT_LAYOUT_JAPANESE) {
        switch (ch) {
        case '@':
            key = HID_USAGE_KEY_KEYBOARD_LEFT_BRACKET_AND_LEFT_BRACE;
            break;
        case '^':
            key = HID_USAGE_KEY_KEYBOARD_EQUAL_AND_PLUS;
            break;
        case '&':
            key = HID_USAGE_KEY_KEYBOARD_6_AND_CARET;
            mods = MOD_LSFT;
            break;
        case '*':
            key = HID_USAGE_KEY_KEYBOARD_APOSTROPHE_AND_QUOTE;
            mods = MOD_LSFT;
            break;
        case '(':
            key = HID_USAGE_KEY_KEYBOARD_8_AND_ASTERISK;
            mods = MOD_LSFT;
            break;
        case ')':
            key = HID_USAGE_KEY_KEYBOARD_9_AND_LEFT_PARENTHESIS;
            mods = MOD_LSFT;
            break;
        case '_':
            key = HID_USAGE_KEY_KEYBOARD_INTERNATIONAL1;
            mods = MOD_LSFT;
            break;
        case '=':
            key = HID_USAGE_KEY_KEYBOARD_MINUS_AND_UNDERSCORE;
            mods = MOD_LSFT;
            break;
        case '+':
            key = HID_USAGE_KEY_KEYBOARD_SEMICOLON_AND_COLON;
            mods = MOD_LSFT;
            break;
        case '[':
            key = HID_USAGE_KEY_KEYBOARD_RIGHT_BRACKET_AND_RIGHT_BRACE;
            break;
        case '{':
            key = HID_USAGE_KEY_KEYBOARD_RIGHT_BRACKET_AND_RIGHT_BRACE;
            mods = MOD_LSFT;
            break;
        case ']':
            key = HID_USAGE_KEY_KEYBOARD_BACKSLASH_AND_PIPE;
            break;
        case '}':
            key = HID_USAGE_KEY_KEYBOARD_BACKSLASH_AND_PIPE;
            mods = MOD_LSFT;
            break;
        case '\\':
            key = HID_USAGE_KEY_KEYBOARD_INTERNATIONAL1;
            break;
        case '|':
            key = HID_USAGE_KEY_KEYBOARD_BACKSLASH_AND_PIPE;
            mods = MOD_LSFT;
            break;
        case ';':
            key = HID_USAGE_KEY_KEYBOARD_SEMICOLON_AND_COLON;
            break;
        case ':':
            key = HID_USAGE_KEY_KEYBOARD_APOSTROPHE_AND_QUOTE;
            break;
        case '\'':
            key = HID_USAGE_KEY_KEYBOARD_7_AND_AMPERSAND;
            mods = MOD_LSFT;
            break;
        case '"':
            key = HID_USAGE_KEY_KEYBOARD_2_AND_AT;
            mods = MOD_LSFT;
            break;
        case '`':
            key = HID_USAGE_KEY_KEYBOARD_LEFT_BRACKET_AND_LEFT_BRACE;
            mods = MOD_LSFT;
            break;
        case '~':
            key = HID_USAGE_KEY_KEYBOARD_EQUAL_AND_PLUS;
            mods = MOD_LSFT;
            break;
        default:
            break;
        }
        if (key) {
            *encoded = APPLY_MODS(mods, ZMK_HID_USAGE(HID_USAGE_KEY, key));
            return true;
        }
    }
    if (ch >= 'a' && ch <= 'z') {
        key = HID_USAGE_KEY_KEYBOARD_A + (ch - 'a');
    } else if (ch >= 'A' && ch <= 'Z') {
        key = HID_USAGE_KEY_KEYBOARD_A + (ch - 'A');
        mods = MOD_LSFT;
    } else if (ch >= '1' && ch <= '9') {
        key = HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION + (ch - '1');
    } else if (ch == '0') {
        key = HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS;
    } else {
        switch (ch) {
        case ' ':
            key = HID_USAGE_KEY_KEYBOARD_SPACEBAR;
            break;
        case '!':
            key = HID_USAGE_KEY_KEYBOARD_1_AND_EXCLAMATION;
            mods = MOD_LSFT;
            break;
        case '@':
            key = HID_USAGE_KEY_KEYBOARD_2_AND_AT;
            mods = MOD_LSFT;
            break;
        case '#':
            key = HID_USAGE_KEY_KEYBOARD_3_AND_HASH;
            mods = MOD_LSFT;
            break;
        case '$':
            key = HID_USAGE_KEY_KEYBOARD_4_AND_DOLLAR;
            mods = MOD_LSFT;
            break;
        case '%':
            key = HID_USAGE_KEY_KEYBOARD_5_AND_PERCENT;
            mods = MOD_LSFT;
            break;
        case '^':
            key = HID_USAGE_KEY_KEYBOARD_6_AND_CARET;
            mods = MOD_LSFT;
            break;
        case '&':
            key = HID_USAGE_KEY_KEYBOARD_7_AND_AMPERSAND;
            mods = MOD_LSFT;
            break;
        case '*':
            key = HID_USAGE_KEY_KEYBOARD_8_AND_ASTERISK;
            mods = MOD_LSFT;
            break;
        case '(':
            key = HID_USAGE_KEY_KEYBOARD_9_AND_LEFT_PARENTHESIS;
            mods = MOD_LSFT;
            break;
        case ')':
            key = HID_USAGE_KEY_KEYBOARD_0_AND_RIGHT_PARENTHESIS;
            mods = MOD_LSFT;
            break;
        case '-':
            key = HID_USAGE_KEY_KEYBOARD_MINUS_AND_UNDERSCORE;
            break;
        case '_':
            key = HID_USAGE_KEY_KEYBOARD_MINUS_AND_UNDERSCORE;
            mods = MOD_LSFT;
            break;
        case '=':
            key = HID_USAGE_KEY_KEYBOARD_EQUAL_AND_PLUS;
            break;
        case '+':
            key = HID_USAGE_KEY_KEYBOARD_EQUAL_AND_PLUS;
            mods = MOD_LSFT;
            break;
        case '[':
            key = HID_USAGE_KEY_KEYBOARD_LEFT_BRACKET_AND_LEFT_BRACE;
            break;
        case '{':
            key = HID_USAGE_KEY_KEYBOARD_LEFT_BRACKET_AND_LEFT_BRACE;
            mods = MOD_LSFT;
            break;
        case ']':
            key = HID_USAGE_KEY_KEYBOARD_RIGHT_BRACKET_AND_RIGHT_BRACE;
            break;
        case '}':
            key = HID_USAGE_KEY_KEYBOARD_RIGHT_BRACKET_AND_RIGHT_BRACE;
            mods = MOD_LSFT;
            break;
        case '\\':
            key = HID_USAGE_KEY_KEYBOARD_BACKSLASH_AND_PIPE;
            break;
        case '|':
            key = HID_USAGE_KEY_KEYBOARD_BACKSLASH_AND_PIPE;
            mods = MOD_LSFT;
            break;
        case ';':
            key = HID_USAGE_KEY_KEYBOARD_SEMICOLON_AND_COLON;
            break;
        case ':':
            key = HID_USAGE_KEY_KEYBOARD_SEMICOLON_AND_COLON;
            mods = MOD_LSFT;
            break;
        case '\'':
            key = HID_USAGE_KEY_KEYBOARD_APOSTROPHE_AND_QUOTE;
            break;
        case '"':
            key = HID_USAGE_KEY_KEYBOARD_APOSTROPHE_AND_QUOTE;
            mods = MOD_LSFT;
            break;
        case '`':
            key = HID_USAGE_KEY_KEYBOARD_GRAVE_ACCENT_AND_TILDE;
            break;
        case '~':
            key = HID_USAGE_KEY_KEYBOARD_GRAVE_ACCENT_AND_TILDE;
            mods = MOD_LSFT;
            break;
        case ',':
            key = HID_USAGE_KEY_KEYBOARD_COMMA_AND_LESS_THAN;
            break;
        case '<':
            key = HID_USAGE_KEY_KEYBOARD_COMMA_AND_LESS_THAN;
            mods = MOD_LSFT;
            break;
        case '.':
            key = HID_USAGE_KEY_KEYBOARD_PERIOD_AND_GREATER_THAN;
            break;
        case '>':
            key = HID_USAGE_KEY_KEYBOARD_PERIOD_AND_GREATER_THAN;
            mods = MOD_LSFT;
            break;
        case '/':
            key = HID_USAGE_KEY_KEYBOARD_SLASH_AND_QUESTION_MARK;
            break;
        case '?':
            key = HID_USAGE_KEY_KEYBOARD_SLASH_AND_QUESTION_MARK;
            mods = MOD_LSFT;
            break;
        default:
            return false;
        }
    }
    *encoded = APPLY_MODS(mods, ZMK_HID_USAGE(HID_USAGE_KEY, key));
    return true;
}

static void macro_work_handler(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct macro_runner *current = CONTAINER_OF(delayable, struct macro_runner, work);
    k_mutex_lock(&macro_mutex, K_FOREVER);
    if (current->state == RUNNER_IDLE) {
        k_mutex_unlock(&macro_mutex);
        return;
    }
    LOG_DBG("Macro runner step state=%u slot=%u instruction=%u/%u wait=%u", current->state,
            current->slot, current->cursor, current->end, current->wait);
    if (current->cancel_requested || (int32_t)(k_uptime_get_32() - current->deadline) >= 0) {
        LOG_ERR("Macro runner abort cancel=%u now=%u deadline=%u result=%d",
                current->cancel_requested, k_uptime_get_32(), current->deadline, current->result);
        begin_cleanup(current->cancel_requested ? -ECANCELED : -ETIMEDOUT);
    }
    if (current->state == RUNNER_CLEANUP) {
        if (!current->held_count) {
            finish_runner();
            k_mutex_unlock(&macro_mutex);
            return;
        }
        uint8_t index = current->held_count - 1;
        if (current->held[index].suppressed) {
            current->held_count--;
            schedule_interval();
            k_mutex_unlock(&macro_mutex);
            return;
        }
        int err = queue_event(&current->held[index].effective_binding, false);
        if (err) {
            schedule_retry();
            k_mutex_unlock(&macro_mutex);
            return;
        }
        current->held_count--;
        schedule_interval();
        k_mutex_unlock(&macro_mutex);
        return;
    }
    if (current->wait == RUNNER_WAIT_TAP_RELEASE || current->wait == RUNNER_WAIT_TEXT_RELEASE) {
        int held = held_find(&current->pending_binding);
        if (held < 0) {
            begin_cleanup(-ENOENT);
            k_mutex_unlock(&macro_mutex);
            return;
        }
        if (held >= 0 && current->held[held].suppressed) {
            remove_held(held);
        } else if (queue_event(&current->held[held].effective_binding, false)) {
            schedule_retry();
            k_mutex_unlock(&macro_mutex);
            return;
        } else if (held >= 0) {
            remove_held(held);
        }
        if (current->wait == RUNNER_WAIT_TEXT_RELEASE) {
            current->text_index++;
            const struct runtime_instruction *instruction = &current->instruction;
            if (current->text_index < instruction->length) {
                current->wait = RUNNER_WAIT_TEXT_PRESS;
            } else {
                current->text_index = 0;
                current->wait = RUNNER_WAIT_NONE;
                advance_instruction(current);
            }
            schedule_interval();
        } else {
            current->wait = RUNNER_WAIT_NONE;
            advance_instruction(current);
            schedule_action_delay(current->wait_ms);
        }
        k_mutex_unlock(&macro_mutex);
        return;
    }
    if (current->wait == RUNNER_WAIT_PAUSE_FOR_RELEASE) {
        uint32_t remaining = current->deadline - k_uptime_get_32();
        current->state = RUNNER_WAITING;
        k_work_reschedule(&current->work, K_MSEC(remaining));
        k_mutex_unlock(&macro_mutex);
        return;
    }
    current->wait = RUNNER_WAIT_NONE;
    if (current->cursor >= current->end) {
        begin_cleanup(0);
        k_mutex_unlock(&macro_mutex);
        return;
    }

    if (!current->instruction_loaded) {
        int err = decode_next_instruction(current->record, current->record_length,
                                          &current->record_offset, &current->instruction);
        if (err) {
            begin_cleanup(err);
            k_mutex_unlock(&macro_mutex);
            return;
        }
        current->instruction_loaded = true;
    }
    const struct runtime_instruction *instruction = &current->instruction;
    LOG_DBG("Macro instruction opcode=%u length=%u value1=0x%08x", instruction->opcode,
            instruction->length, instruction->value1);
    switch (instruction->opcode) {
    case DYNAMIC_MACRO_ACTION_WAIT:
        advance_instruction(current);
        current->state = RUNNER_WAITING;
        k_work_reschedule(&current->work, K_MSEC(instruction->value1));
        k_mutex_unlock(&macro_mutex);
        return;
    case DYNAMIC_MACRO_ACTION_PAUSE_FOR_RELEASE:
        advance_instruction(current);
        if (current->trigger_released) {
            schedule_interval();
        } else {
            current->wait = RUNNER_WAIT_PAUSE_FOR_RELEASE;
            current->state = RUNNER_WAITING;
            uint32_t remaining = current->deadline - k_uptime_get_32();
            k_work_reschedule(&current->work, K_MSEC(remaining));
        }
        k_mutex_unlock(&macro_mutex);
        return;
    case DYNAMIC_MACRO_ACTION_SET_TAP_MS:
        current->tap_ms = instruction->value1;
        advance_instruction(current);
        schedule_interval();
        k_mutex_unlock(&macro_mutex);
        return;
    case DYNAMIC_MACRO_ACTION_SET_WAIT_MS:
        current->wait_ms = instruction->value1;
        advance_instruction(current);
        schedule_interval();
        k_mutex_unlock(&macro_mutex);
        return;
    case DYNAMIC_MACRO_ACTION_TEXT_ASCII: {
        uint8_t ch = current->record[instruction->value1 + current->text_index];
        uint32_t encoded;
        LOG_DBG("Macro Text raw byte=%u offset=%u value=0x%02x", current->text_index,
                instruction->value1 + current->text_index, ch);
        if (!char_to_key(ch, current->text_layout, &encoded)) {
            begin_cleanup(-EINVAL);
            break;
        }
        uint8_t external = keymap_held_modifiers();
        if (external) {
            encoded = STRIP_MODS(encoded);
        }
        LOG_DBG("Macro Text byte=%u char=0x%02x key=0x%08x external_mods=0x%02x",
                current->text_index, ch, encoded, external);
        const char *key_name = zmk_behavior_find_behavior_name_from_local_id(macro_key_press_id);
        if (!key_name || runner.held_count >= MACRO_HELD_MAX) {
            begin_cleanup(-ENOSPC);
            break;
        }
        current->pending_binding = (struct zmk_behavior_binding){
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
            .local_id = macro_key_press_id,
#endif
            .behavior_dev = key_name,
            .param1 = encoded,
        };
        current->pending_binding.param1 = encoded;
        if (queue_event(&current->pending_binding, true)) {
            schedule_retry();
            break;
        }
        if (add_held(&current->pending_binding, &current->pending_binding, false)) {
            begin_cleanup(-ENOSPC);
            break;
        }
        current->wait = RUNNER_WAIT_TEXT_RELEASE;
        schedule_interval();
        k_mutex_unlock(&macro_mutex);
        return;
    }
    case DYNAMIC_MACRO_ACTION_BEHAVIOR_TAP:
    case DYNAMIC_MACRO_ACTION_BEHAVIOR_PRESS:
    case DYNAMIC_MACRO_ACTION_BEHAVIOR_RELEASE: {
        struct zmk_behavior_binding binding = instruction_binding(instruction);
        if (!binding.behavior_dev) {
            begin_cleanup(-ENOENT);
            break;
        }
        int held = held_find(&binding);
        if (instruction->opcode == DYNAMIC_MACRO_ACTION_BEHAVIOR_RELEASE) {
            if (held < 0) {
                begin_cleanup(-ENOENT);
                break;
            }
            if (current->held[held].suppressed) {
                remove_held(held);
                advance_instruction(current);
                schedule_action_delay(current->wait_ms);
                k_mutex_unlock(&macro_mutex);
                return;
            }
            if (queue_event(&current->held[held].effective_binding, false)) {
                schedule_retry();
                break;
            }
            remove_held(held);
            advance_instruction(current);
            schedule_action_delay(current->wait_ms);
            k_mutex_unlock(&macro_mutex);
            return;
        }
        uint8_t external = keymap_held_modifiers();
        uint8_t page = ZMK_HID_USAGE_PAGE(binding.param1);
        uint16_t keycode = ZMK_HID_USAGE_ID(binding.param1);
        bool is_modifier_key = page == HID_USAGE_KEY && is_mod(page, keycode);
        bool suppressed = is_modifier_key && external;
        if (suppressed) {
            if (instruction->opcode == DYNAMIC_MACRO_ACTION_BEHAVIOR_PRESS) {
                add_held(&binding, &binding, true);
            }
            advance_instruction(current);
            schedule_action_delay(current->wait_ms);
            k_mutex_unlock(&macro_mutex);
            return;
        }
        struct zmk_behavior_binding effective_binding = binding;
        if (external && SELECT_MODS(binding.param1)) {
            effective_binding.param1 = STRIP_MODS(binding.param1);
        }
        if (queue_event(&effective_binding, true)) {
            schedule_retry();
            break;
        }
        if (add_held(&binding, &effective_binding, false)) {
            begin_cleanup(-ENOSPC);
            break;
        }
        if (instruction->opcode == DYNAMIC_MACRO_ACTION_BEHAVIOR_TAP) {
            current->pending_binding = binding;
            current->wait = RUNNER_WAIT_TAP_RELEASE;
            schedule_action_delay(current->tap_ms);
        } else {
            advance_instruction(current);
            schedule_action_delay(current->wait_ms);
        }
        k_mutex_unlock(&macro_mutex);
        return;
    }
    default:
        begin_cleanup(-ENOTSUP);
        break;
    }
    if (current->state == RUNNER_CLEANUP) {
        k_work_reschedule(&current->work, K_NO_WAIT);
    }
    k_mutex_unlock(&macro_mutex);
}

static int macro_behavior_init(const struct device *dev, uint16_t slot) {
    if (slot >= MACRO_SLOT_COUNT) {
        return -EINVAL;
    }
    macro_slots[slot].device = dev;
    return 0;
}

static int macro_settings_load_impl(void) {
    for (uint16_t slot = 0; slot < MACRO_SLOT_COUNT; slot++) {
        if (!macro_slots[slot].device || !device_is_ready(macro_slots[slot].device)) {
            return -ENODEV;
        }
        for (size_t i = 0; i < dynamic_setting_group_count; i++) {
            const struct dynamic_setting_group_descriptor *group = &dynamic_setting_groups[i];
            if (group->slot_index != slot) {
                continue;
            }
            if (!macro_record_group(group)) {
                continue;
            }
            if ((macro_slots[slot].settings && macro_slots[slot].settings != group) ||
                group->scope != DYNAMIC_SETTING_SCOPE_SHARED || group->setting_count ||
                group->record_max_length == 0 ||
                group->record_max_length > DYNAMIC_MACRO_MAX_RECORD_LENGTH) {
                return -EINVAL;
            }
            macro_slots[slot].settings = group;
        }
        if (!macro_slots[slot].settings) {
            LOG_ERR("Macro settings group missing for slot %u", slot);
            return -ENOENT;
        }
    }
    macro_key_press_id = zmk_behavior_get_local_id(DEVICE_DT_NAME(DT_NODELABEL(kp)));
    if (macro_key_press_id == UINT16_MAX) {
        LOG_ERR("Macro initialization: kp Behavior local ID is not loaded yet");
        return -ENOENT;
    }
    for (uint16_t slot = 0; slot < MACRO_SLOT_COUNT; slot++) {
        const uint8_t *record;
        size_t length;
        uint16_t count;
        int err = group_record(macro_slots[slot].settings, NULL, NULL, 0, &record, &length);
        if (!err) {
            err =
                decode_record(record, length, true, &count, NULL, NULL, NULL, NULL, NULL, 0, NULL);
        }
        if (err) {
            LOG_ERR("Macro slot validation failed slot=%u err=%d", slot, err);
            return err;
        }
    }
    return 0;
}

/* Defer cache construction until ZMK has completed settings_load(). */
static int macro_settings_ensure_ready(void) {
    if (macro_settings_ready) {
        return 0;
    }
    int err = macro_settings_load_impl();
    if (err) {
        LOG_ERR("Macro settings initialization failed: %d", err);
    } else {
        macro_settings_ready = true;
        LOG_INF("Macro settings ready: %u slots, up to %u instructions per slot", MACRO_SLOT_COUNT,
                DYNAMIC_MACRO_MAX_INSTRUCTIONS);
    }
    return err;
}

dynamic_setting_record_on_change_fn dynamic_macro_record_changed;
dynamic_setting_on_change_fn dynamic_macro_text_layout_changed;
int dynamic_macro_text_layout_changed(const struct dynamic_setting_descriptor *descriptor,
                                      const struct dynamic_setting_value *new_value,
                                      enum dynamic_setting_change_reason reason,
                                      const void *context) {
    ARG_UNUSED(descriptor);
    ARG_UNUSED(reason);
    ARG_UNUSED(context);
    if (!new_value || new_value->type != DYNAMIC_SETTING_TYPE_ENUM || new_value->length != 1 ||
        new_value->data[0] > MACRO_TEXT_LAYOUT_JAPANESE) {
        return -EINVAL;
    }
    k_mutex_lock(&macro_mutex, K_FOREVER);
    macro_text_layout = (enum macro_text_layout)new_value->data[0];
    k_mutex_unlock(&macro_mutex);
    return 0;
}

int dynamic_macro_record_changed(const struct dynamic_setting_group_descriptor *group,
                                 const uint8_t *record, size_t length,
                                 enum dynamic_setting_change_reason reason, const void *context) {
    ARG_UNUSED(reason);
    ARG_UNUSED(context);
    if (!macro_record_group(group) || !record) {
        return -EINVAL;
    }
    k_mutex_lock(&macro_mutex, K_FOREVER);
    if (!macro_settings_ready) {
        int validate_err =
            decode_record(record, length, false, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL);
        k_mutex_unlock(&macro_mutex);
        return validate_err;
    }
    int err = decode_record(record, length, true, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL);
    if (err) {
        LOG_ERR("Macro record update rejected: %d", err);
    } else {
        uint16_t slot = 0;
        while (slot < MACRO_SLOT_COUNT && macro_slots[slot].settings != group) {
            slot++;
        }
        LOG_INF("Macro slot %u record accepted: %u bytes", slot, length);
    }
    k_mutex_unlock(&macro_mutex);
    return err;
}

static int macro_pressed(struct zmk_behavior_binding *binding,
                         struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (!dev) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    uint16_t slot = 0;
    while (slot < MACRO_SLOT_COUNT && macro_slots[slot].device != dev) {
        slot++;
    }
    if (slot == MACRO_SLOT_COUNT) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    k_mutex_lock(&macro_mutex, K_FOREVER);
    int err = macro_settings_ensure_ready();
    if (err) {
        k_mutex_unlock(&macro_mutex);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    if (runner.state != RUNNER_IDLE) {
        k_mutex_unlock(&macro_mutex);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    k_mutex_unlock(&macro_mutex);

    const uint8_t *record = NULL;
    size_t record_length = 0;
    err = pin_macro_records(slot, &record, &record_length);
    if (err) {
        LOG_ERR("Macro record pin failed: %d", err);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    k_mutex_lock(&macro_mutex, K_FOREVER);
    if (runner.state != RUNNER_IDLE) {
        k_mutex_unlock(&macro_mutex);
        unpin_macro_records();
        return ZMK_BEHAVIOR_OPAQUE;
    }
    uint16_t instruction_count = 0;
    struct macro_slot_timing timing = {0};
    err = decode_record(record, record_length, true, &instruction_count, NULL, NULL, &timing, NULL,
                        NULL, 0, NULL);
    if (err) {
        LOG_ERR("Macro slot %u load failed: %d", slot, err);
        k_mutex_unlock(&macro_mutex);
        unpin_macro_records();
        return ZMK_BEHAVIOR_OPAQUE;
    }
    runner.slot = slot;
    runner.record = record;
    runner.record_length = record_length;
    runner.record_offset = MACRO_RECORD_HEADER_SIZE;
    runner.instruction_loaded = false;
    runner.tap_ms = timing.tap_ms;
    runner.wait_ms = timing.wait_ms;
    runner.cursor = 0;
    runner.end = instruction_count;
    runner.event = event;
    runner.held_count = 0;
    runner.text_index = 0;
    runner.text_layout = macro_text_layout;
    runner.wait = RUNNER_WAIT_NONE;
    runner.trigger_released = false;
    runner.cancel_requested = false;
    runner.result = 0;
    runner.deadline = k_uptime_get_32() + DYNAMIC_MACRO_MAX_DURATION_MS;
    runner.state = RUNNER_RUNNING;
    LOG_INF("Macro slot %u triggered: %u instructions, %u record bytes", slot, instruction_count,
            record_length);
    k_work_reschedule(&runner.work, K_NO_WAIT);
    k_mutex_unlock(&macro_mutex);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int macro_released(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (!dev) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    uint16_t slot = 0;
    while (slot < MACRO_SLOT_COUNT && macro_slots[slot].device != dev) {
        slot++;
    }
    if (slot == MACRO_SLOT_COUNT) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    k_mutex_lock(&macro_mutex, K_FOREVER);
    bool matching_trigger = runner.state != RUNNER_IDLE && runner.slot == slot &&
                            runner.event.position == event.position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    matching_trigger = matching_trigger && runner.event.source == event.source;
#endif
    if (matching_trigger) {
        runner.trigger_released = true;
        if (runner.wait == RUNNER_WAIT_PAUSE_FOR_RELEASE) {
            runner.wait = RUNNER_WAIT_NONE;
            runner.state = RUNNER_RUNNING;
            k_work_reschedule(&runner.work, K_NO_WAIT);
        }
    }
    k_mutex_unlock(&macro_mutex);
    return ZMK_BEHAVIOR_OPAQUE;
}

static void cancel_active_runner(void) {
    k_mutex_lock(&macro_mutex, K_FOREVER);
    if (runner.state != RUNNER_IDLE) {
        LOG_WRN("Macro runner cancellation requested state=%u slot=%u", runner.state, runner.slot);
        runner.cancel_requested = true;
        k_work_reschedule(&runner.work, K_NO_WAIT);
    }
    k_mutex_unlock(&macro_mutex);
}

static int macro_endpoint_listener(const zmk_event_t *event) {
    struct zmk_endpoint_changed *changed = as_zmk_endpoint_changed(event);
    LOG_INF("Macro observed endpoint change to %u",
            changed ? changed->endpoint.transport : UINT8_MAX);
    cancel_active_runner();
    return ZMK_EV_EVENT_BUBBLE;
}

static int macro_split_status_listener(const zmk_event_t *event) {
    const struct zmk_split_peripheral_status_changed *status =
        as_zmk_split_peripheral_status_changed(event);
    if (status && !status->connected) {
        LOG_WRN("Macro observed split peripheral disconnect");
        cancel_active_runner();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dynamic_macro_endpoint, macro_endpoint_listener);
ZMK_SUBSCRIPTION(dynamic_macro_endpoint, zmk_endpoint_changed);
ZMK_LISTENER(dynamic_macro_split, macro_split_status_listener);
ZMK_SUBSCRIPTION(dynamic_macro_split, zmk_split_peripheral_status_changed);

static const struct behavior_driver_api macro_api = {
    .binding_pressed = macro_pressed,
    .binding_released = macro_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define MACRO_INST(inst)                                                                           \
    static int macro_init_##inst(const struct device *dev) {                                       \
        return macro_behavior_init(dev, inst);                                                     \
    }                                                                                              \
    BEHAVIOR_DT_INST_DEFINE(inst, macro_init_##inst, NULL, NULL, NULL, POST_KERNEL,                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &macro_api);

DT_INST_FOREACH_STATUS_OKAY(MACRO_INST)

static int macro_work_init(void) {
    k_work_init_delayable(&runner.work, macro_work_handler);
    return 0;
}
SYS_INIT(macro_work_init, APPLICATION, 0);
