"""Shared stick-curve presets, validation and legacy conversion for all tools."""
POINT_COUNT = 15
PRESET_NAMES = ('linear', 'concave', 's-curve')


def validate(points, count=POINT_COUNT):
    points = bytes(points)
    if len(points) != count or any(a > b for a, b in zip(points, points[1:])):
        raise ValueError(f'A curve needs {count} nondecreasing output bytes (0–255)')
    return points


def preset(name, count=POINT_COUNT):
    values = []
    for i in range(1, count + 1):
        x = i / (count + 1)
        y = x if name == 'linear' else x*x if name == 'concave' else (2*x*(1-x) if x <= .5 else 1-2*(1-x)*x)
        values.append(round(y * 255))
    if name not in PRESET_NAMES:
        raise ValueError(f'Unknown curve preset: {name}')
    return validate(values, count)


def identify(points):
    return next((name for name in PRESET_NAMES if bytes(points) == preset(name, len(points))), 'custom')


def resample(points, count=POINT_COUNT):
    points = validate(points, len(points))
    source = bytes([0]) + points + bytes([255])
    segments = len(points) + 1
    result = []
    for i in range(1, count + 1):
        index, remainder = divmod(i * segments, count + 1)
        result.append((source[index] * (count + 1 - remainder) + source[index + 1] * remainder + (count + 1)//2) // (count + 1))
    return validate(result, count)


def migrate_slot(values, count=POINT_COUNT):
    """Convert a backup's legacy modes/points; retain inactive custom points."""
    values = {name: dict(value) for name, value in values.items()}
    for side in ('left', 'right'):
        mode = values.pop(f'{side}_curve', None)
        point_name, enabled_name = f'{side}_curve_points', f'{side}_curve_enabled'
        stored = values.get(point_name)
        old_points = bytes(int(v) for v in stored['value'].split(',')) if stored and stored.get('set') else preset('linear', count)
        if mode:
            selected = mode['value'] if mode.get('set') else 'linear'
            points = preset(selected, count) if selected in ('concave', 's-curve') else resample(old_points, count)
            values[enabled_name] = {'value': 'enabled' if selected != 'linear' else 'disabled', 'set': selected != 'linear'}
            values[point_name] = {'value': ','.join(map(str, points)), 'set': bool(stored and stored.get('set')) or selected in ('concave', 's-curve')}
        elif stored:
            points = resample(old_points, count)
            values[point_name]['value'] = ','.join(map(str, points))
    return values
