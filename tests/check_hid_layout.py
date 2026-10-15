# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Source-only ABI arithmetic check against pinned Eden declarations. No compiler is invoked."""

import argparse
import re
import subprocess
from pathlib import Path

EDEN_REVISION = '6352612425ae0765802eaa8c5904ff652b97222a'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--eden', required=True, type=Path)
    args = parser.parse_args()
    revision = subprocess.check_output(['git', '-C', str(args.eden), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != EDEN_REVISION:
        raise SystemExit(f'Expected Eden {EDEN_REVISION}, got {revision}')
    sources = [
        'src/hid_core/resources/shared_memory_format.h',
        'src/hid_core/resources/ring_lifo.h',
        'src/hid_core/hid_types.h',
        'src/hid_core/resources/npad/npad_types.h',
        'src/hid_core/resources/keyboard/keyboard_types.h',
        'src/hid_core/resources/debug_pad/debug_pad_types.h',
        'src/hid_core/resources/touch_screen/touch_types.h',
    ]
    subprocess.run(
        ['git', '-C', str(args.eden), 'diff', '--exit-code', 'HEAD', '--', *sources],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    text = '\n'.join((args.eden / path).read_text() for path in sources)
    sizes = {
        name: int(value, 0)
        for name, value in re.findall(r'static_assert\(sizeof\((\w+)\)\s*==\s*(0x[0-9A-Fa-f]+|\d+)', text)
    }
    count = int(re.search(r'HidEntryCount\s*=\s*(\d+)', text).group(1))
    expected = {}

    def size(local, reference):
        expected[f'HS_{local}'] = sizes[reference]

    def offsets(local, **fields):
        for field, offset in fields.items():
            expected[f'HO_{local}_{field}'] = offset

    def lifo(state):
        return sizes['CommonHeader'] + count * (8 + sizes[state])

    for local, reference in {
        'HidSharedMemory': 'SharedMemoryFormat',
        'HidNpadSharedMemoryEntry': 'NpadSharedMemoryEntry',
        'HidNpadInternalState': 'NpadInternalState',
        'HidNpadCommonState': 'NPadGenericState',
        'HidCommonLifoHeader': 'CommonHeader',
        'HidSixAxisSensorState': 'SixAxisSensorState',
        'HidTouchScreenState': 'TouchScreenState',
        'HidTouchState': 'TouchState',
        'HidNpadControllerColor': 'NpadControllerColor',
        'HidNpadFullKeyColorState': 'NpadFullKeyColorState',
        'HidNpadJoyColorState': 'NpadJoyColorState',
        'HidKeyboardState': 'KeyboardState',
        'HidMouseState': 'MouseState',
        'HidDebugPadState': 'DebugPadState',
    }.items():
        size(local, reference)
    for local, reference in {
        'HidNpadCommon': 'NPadGenericState',
        'HidNpadSixAxisSensor': 'SixAxisSensorState',
        'HidTouchScreen': 'TouchScreenState',
    }.items():
        expected[f'HS_{local}Lifo'] = lifo(reference)
        offsets(f'{local}Lifo', storage=sizes['CommonHeader'])
    for local, reference in {
        'HidNpadCommonState': 'NPadGenericState',
        'HidSixAxisSensorState': 'SixAxisSensorState',
        'HidTouchScreenState': 'TouchScreenState',
        'HidNpadGcTriggerState': 'NpadGcTriggerState',
    }.items():
        expected[f'HS_{local}AtomicStorage'] = 8 + sizes[reference]
    offsets('HidNpadGcTriggerLifo', storage=sizes['CommonHeader'])
    offsets('HidNpadCommonStateAtomicStorage', sampling_number=0, state=8)
    offsets('HidCommonLifoHeader', unused=0, buffer_count=8, tail=16, count=24)
    offsets('HidNpadCommonState', sampling_number=0, buttons=8, analog_stick_l=16, analog_stick_r=24, attributes=32)
    offsets('HidSixAxisSensorState', acceleration=16, direction=16 + 3 * 12, attributes=16 + 6 * 12)
    offsets('HidTouchScreenState', count=8, touches=16)
    # TouchState: u64 delta_time, u32 attribute, u32 finger, a Point<u32>, two u32 diameters, s32 rotation_angle.
    offsets(
        'HidTouchState',
        attribute=8,
        finger=12,
        position_x=16,
        position_y=20,
        diameter_x=24,
        diameter_y=28,
        rotation_angle=32,
    )
    if (36 + 7) & ~7 != sizes['TouchState']:
        raise SystemExit("TouchState field arithmetic disagrees with Eden's size assertion")
    for name in ('TouchScreen', 'Keyboard', 'Mouse'):
        offsets(f'Hid{name}SharedMemoryFormat', lifo=0)

    # Ordered fields in Eden's SharedMemoryFormat, through ConsoleSixAxisSensor.
    position = 0
    for field, reference in (
        ('debug_pad', 'DebugPadSharedMemoryFormat'),
        ('touchscreen', 'TouchScreenSharedMemoryFormat'),
        ('mouse', 'MouseSharedMemoryFormat'),
        ('keyboard', 'KeyboardSharedMemoryFormat'),
        (None, 'DigitizerSharedMemoryFormat'),
        ('home_button', 'HomeButtonSharedMemoryFormat'),
        (None, 'SleepButtonSharedMemoryFormat'),
        ('capture_button', 'CaptureButtonSharedMemoryFormat'),
        (None, 'InputDetectorSharedMemoryFormat'),
        ('unique_pad', 'UniquePadSharedMemoryFormat'),
        ('npad', 'NpadSharedMemoryFormat'),
        ('gesture', 'GestureSharedMemoryFormat'),
        ('console_six_axis_sensor', 'ConsoleSixAxisSensorSharedMemoryFormat'),
    ):
        if field:
            offsets('HidSharedMemory', **{field: position})
        position += sizes[reference]

    offsets(
        'HidNpadInternalState',
        style_set=0,
        joy_assignment_mode=4,
        full_key_color=8,
        joy_color=8 + sizes['NpadFullKeyColorState'],
    )
    position = 8 + sizes['NpadFullKeyColorState'] + sizes['NpadJoyColorState']
    for field in ('full_key', 'handheld', 'joy_dual', 'joy_left', 'joy_right', 'palma', 'system_ext'):
        offsets('HidNpadInternalState', **{f'{field}_lifo': position})
        position += lifo('NPadGenericState')
    for field in ('full_key', 'handheld', 'joy_dual_left', 'joy_dual_right', 'joy_left', 'joy_right'):
        offsets('HidNpadInternalState', **{f'{field}_six_axis_sensor_lifo': position})
        position += lifo('SixAxisSensorState')
    offsets(
        'HidNpadInternalState',
        device_type=position,
        system_properties=position + 8,
        system_button_properties=position + 16,
        battery_level=position + 20,
        applet_footer_ui_attribute=position + 32,
        applet_footer_ui_type=position + 36,
    )
    # u8 footer, 0x5b reserved bytes, 0x20 unknown bytes, then the GC trigger lifo.
    position += 36 + 1 + 0x5B + 0x20
    offsets('HidNpadInternalState', gc_trigger_lifo=position)
    position += lifo('NpadGcTriggerState')
    offsets('HidNpadInternalState', lark_type_l_and_main=position)
    # Four u32 type fields and six u8 sensor properties, rounded to 8-byte struct alignment.
    if (position + 16 + 6 * sizes['SixAxisSensorProperties'] + 7) & ~7 != sizes['NpadInternalState']:
        raise SystemExit("NpadInternalState tail arithmetic disagrees with Eden's size assertion")

    header = Path(__file__).resolve().parents[1] / 'src/nx/hle/services/hid_layout.h'
    actual = {
        name: int(value, 0)
        for name, value in re.findall(
            r'^#define\s+((?:HS|HO)_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\s*$', header.read_text(), re.MULTILINE
        )
    }
    errors = []
    for name in sorted(actual.keys() | expected.keys()):
        if actual.get(name) != expected.get(name):
            errors.append(f'{name}: header={actual.get(name)!r}, reference={expected.get(name)!r}')
    if errors:
        raise SystemExit('\n'.join(errors))
    print(f'PASS source-only HID layout arithmetic: {len(actual)} constants (no build or runtime test)')


if __name__ == '__main__':
    main()
