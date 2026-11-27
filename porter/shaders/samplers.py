import math


def decode_air_sampler(value):
    """Decode the supported AIR constexpr encoding before runtime packaging."""
    raw = int(value, 0) if isinstance(value, str) else value
    fields = 0x1ff | (3 << 9) | (3 << 11) | 0x8000
    if not isinstance(raw, int) or raw < 0 or raw & ~fields != 0x8000000000080000:
        raise ValueError('Unsupported AIR sampler encoding')
    address = tuple((raw >> shift) & 7 for shift in (0, 3, 6))
    mag, minimum = (raw >> 9) & 3, (raw >> 11) & 3
    if max(address) > 3 or mag > 1 or minimum > 1:
        raise ValueError('Unsupported AIR sampler fields')
    return dict(min_filter=minimum, mag_filter=mag, address=address,
                pixel_coordinates=bool(raw & 0x8000))


def sampler_description(literal):
    """Return portable sampler fields; plans can supply an explicit descriptor."""
    descriptor = literal.get('descriptor')
    if descriptor is None:
        decoded = decode_air_sampler(literal['raw'])
        # AIR lowering normalizes pixel coordinates in the shader itself.
        address_names = ('border', 'edge', 'repeat', 'mirror')
        descriptor = dict(min_filter=('nearest', 'linear')[decoded['min_filter']],
                          mag_filter=('nearest', 'linear')[decoded['mag_filter']],
                          address=[address_names[value] for value in decoded['address']])
    filters = {'nearest': 0, 'linear': 1}
    addresses = {'repeat': 0, 'mirror': 1, 'edge': 2, 'border': 3}
    borders = {'transparent_black': 0, 'opaque_black': 2, 'opaque_white': 4}
    allowed = {'min_filter', 'mag_filter', 'mip_filter', 'address', 'lod_min', 'lod_max', 'border'}
    if not isinstance(descriptor, dict) or descriptor.keys() - allowed:
        raise ValueError('Unknown literal sampler descriptor fields')
    address = descriptor.get('address', ['edge'] * 3)
    if len(address) != 3:
        raise ValueError('A literal sampler needs three address modes')
    try:
        result = [filters[descriptor.get('min_filter', 'nearest')],
                  filters[descriptor.get('mag_filter', 'nearest')],
                  filters[descriptor.get('mip_filter', 'nearest')],
                  *(addresses[value] for value in address),
                  borders[descriptor.get('border', 'transparent_black')]]
    except KeyError as error:
        raise ValueError('Unsupported literal sampler field: ' + str(error)) from error
    lod_min, lod_max = (float(descriptor.get(name, 0)) for name in ('lod_min', 'lod_max'))
    if not math.isfinite(lod_min) or not math.isfinite(lod_max) or lod_min < -3.402823466e38 or lod_max < lod_min or lod_max > 3.402823466e38:
        raise ValueError('Invalid literal sampler LOD range')
    return (*result, lod_min, lod_max)
