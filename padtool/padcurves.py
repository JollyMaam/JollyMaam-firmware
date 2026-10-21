"""Shared stick-curve presets and validation for all tools."""
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

