"""List native opcodes the lowering handles but the register-pressure pass has no operands for.

The pressure pass re-expands an instruction with low shadow copies when it names a high guest
register, and it refuses opcodes whose operand fields it does not know. Its switch has drifted
behind `lower_to_maxwell.h`, so this prints the difference. A finite gap means a rejected shader.

    python tools/nx2/lower_operand_audit.py [--repo PATH]

Add an entry from the lowering's case body: list only the bit fields that body reads as guest
registers. Case groups are not uniform (`0x211` and `0xa11` share a body but read different
fields), so each opcode is judged on its own branch, not on its label group.
"""
import argparse
import re
import sys
from pathlib import Path

CASE = re.compile(r'case (0x[0-9a-f]{3})\s*:')
FIELD = re.compile(r'insn\.Bits\((16|24|32|64),\s*8\)')


def opcodes(text):
    return {int(match.group(1), 16): match.start() for match in CASE.finditer(text)}


def pressure_switch(text):
    start = text.index('switch (opcode)')
    return text[start:text.index('default:', start)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()

    lower_path = args.repo / 'src/shader_recompiler/frontend/ampere/lower_to_maxwell.h'
    pressure_path = args.repo / 'src/shader_recompiler/frontend/ampere/register_pressure.h'
    try:
        lower = lower_path.read_text(encoding='utf-8')
        pressure = pressure_path.read_text(encoding='utf-8')
    except OSError as error:
        print(f'cannot read the lowering: {error}', file=sys.stderr)
        return 2

    lowered = opcodes(lower)
    known = opcodes(pressure_switch(pressure))
    missing = sorted(set(lowered) - set(known))
    print(f'lowering opcodes: {len(lowered)}, pressure opcodes: {len(known)}, missing: {len(missing)}')
    for opcode in missing:
        print(f'0x{opcode:03x}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
