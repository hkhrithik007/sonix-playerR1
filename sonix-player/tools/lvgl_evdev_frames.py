"""Keep pointer SYN_REPORT frames distinct in the supported LVGL evdev driver.

Idempotent; refuse an unknown loop rather than partially patching a dependency.
Used before make evaluates dependency timestamps. No change to polling cadence.
"""
from pathlib import Path
import argparse

BOUNDARY = '\n    }\n\n    if(!dsc->deleting && br == -1 && errno != EAGAIN) {'
FRAME = '''
        /* Sonix: deliver each complete pointer frame before consuming the next.
         * A queued press/release pair must not collapse into RELEASED only.
         * One final empty read ends continue_reading; no idle timer is added. */
        if(in.type == EV_SYN && in.code == SYN_REPORT && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            data->continue_reading = true;
            break;
        }
'''


def patched(text):
    if FRAME in text:
        assert text.count(FRAME) == 1
        return text
    assert text.count(BOUNDARY) == 1, 'Unsupported evdev read loop'
    assert 'while((br = read(dsc->fd, &in, sizeof(in))) > 0)' in text
    return text.replace(BOUNDARY, FRAME + BOUNDARY)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('lvgl', type=Path)
    args = parser.parse_args()
    path = args.lvgl / 'src/drivers/evdev/lv_evdev.c'
    before = path.read_text()
    after = patched(before)
    if before != after:
        path.write_text(after, newline='\n')
    print('ok')
