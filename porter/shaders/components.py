"""Conservative component usage for separate sampled images in validated SPIR-V.

Only narrow direct image -> sample -> scalar literal extract chains. Unknown
instructions, opaque aliases or vector uses leave the binding at all components.
This is a build-time hint; it does not rewrite the shader or its interface.
"""
from collections import defaultdict
import struct


def sampled_component_masks(blob):
    words = struct.unpack('<' + 'I' * (len(blob) // 4), blob)
    decorations, variables, uses = defaultdict(dict), set(), defaultdict(list)
    inside, cursor = False, 5
    while cursor < len(words):
        length, op = divmod(words[cursor], 65536)
        end = cursor + length
        if not length or end > len(words):
            raise ValueError('Invalid SPIR-V instruction length')
        args = words[cursor + 1:end]
        cursor = end
        if op == 54:  # OpFunction
            inside = True
        if op == 56:  # OpFunctionEnd
            inside = False
        if not inside:
            if op == 71 and len(args) == 3 and args[1] in (33, 34):  # Binding, DescriptorSet
                decorations[args[0]][args[1]] = args[2]
            elif op == 59 and len(args) == 3 and args[2] == 0:  # UniformConstant variable
                variables.add(args[1])
            elif op in (73, 74, 75, 332) or (op == 14 and args[0] != 0):
                return {}  # Decoration groups, ID-valued decorations or non-logical addressing.
            elif op == 59 and len(args) > 3:
                uses[args[3]].append((op, args, 3))  # Global pointer initializer.
            continue
        # Include unknown instructions and even literal operands as possible uses:
        # a numerical collision only loses an optimization. Skip known definitions.
        for index, operand in enumerate(args):
            if index == 1 and op in (61, 86, 87, 88):
                continue
            uses[operand].append((op, args, index))

    def mask(variable):
        bits = 0
        for op, load, pos in uses[variable]:
            if op != 61 or pos != 2 or len(load) != 3:  # OpLoad
                return 15
            for op, combined, pos in uses[load[1]]:
                if op != 86 or pos != 2 or len(combined) != 4:  # OpSampledImage (image operand)
                    return 15
                for op, sample, pos in uses[combined[1]]:
                    if op not in (87, 88) or pos != 2:  # Ordinary implicit/explicit LOD samples
                        return 15
                    for op, extract, pos in uses[sample[1]]:
                        if op != 81 or pos != 2 or len(extract) != 4 or extract[3] >= 4:
                            return 15
                        bits |= 1 << extract[3]
        return bits or 15

    result = {}
    for variable, deco in decorations.items():
        if 33 not in deco or 34 not in deco:
            continue
        key = (deco[34], deco[33])
        # Multiple descriptor variables at the same binding are not a proof.
        result[key] = mask(variable) if variable in variables and key not in result else 15
    return {key: value for key, value in result.items() if value != 15}
