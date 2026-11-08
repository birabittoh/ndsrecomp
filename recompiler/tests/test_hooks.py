"""Mid-instruction hooks (docs/midasm-hooks.md), end to end.

Builds a tiny ARM/Thumb image, recompiles it with a hook manifest, compiles
the generated bank as C, links it against a probe that defines the handlers
and a minimal runtime, and executes the hooked functions. Also pins the
emitter's fail-closed checks and the inline-leaf exclusion.
"""
from pathlib import Path
import argparse
import hashlib
import re
import subprocess
import sys

p = argparse.ArgumentParser()
for name in ['recompiler', 'compiler', 'runtime_include', 'core_include', 'work']:
    p.add_argument('--' + name.replace('_', '-'), required=True)
a = p.parse_args()
work = Path(a.work)
work.mkdir(parents=True, exist_ok=True)

BASE = 0x02010000
words = [0xE1A00000] * 64
# fa (ARM leaf): mov r0,#1 ; add r0,r0,r1 ; bx lr
words[0:3] = [0xE3A00001, 0xE0800001, 0xE12FFF1E]
# fb (ARM): mov r2,#5 ; mov r2,#6 ; mov r3,#7 ; bx lr
words[8:12] = [0xE3A02005, 0xE3A02006, 0xE3A03007, 0xE12FFF1E]
# ft (Thumb leaf): movs r0,#1 ; adds r0,r0,r1 ; bx lr
words[16] = 0x2001 | (0x1840 << 16)
words[17] = 0x4770
# fc (ARM caller): push {lr} ; bl fa ; pop {pc}
words[24:27] = [0xE92D4000,
                0xEB000000 | (((0 - (0x64 + 8)) >> 2) & 0xFFFFFF),
                0xE8BD8000]
image = b''.join(w.to_bytes(4, 'little') for w in words)
sha = hashlib.sha1(image).hexdigest()
(work / 'fx.bin').write_bytes(image)
config = f'''[program]
name="hook fixture"
id="hook_fixture"
load_address={BASE}
size={len(image)}
entry_pc={BASE + 0x60}
authoritative_entry_points=true
[identity]
sha1="{sha}"
'''
for off, mode, name, size in [(0x00, 'arm', 'fa', 12), (0x20, 'arm', 'fb', 16),
                              (0x40, 'thumb', 'ft', 6), (0x60, 'arm', 'fc', 12)]:
    config += (f'[[entry_point]]\naddr={BASE + off}\nmode="{mode}"\n'
               f'name="{name}"\nsize={size}\n')
(work / 'fx.toml').write_text(config)

HEADER = f'version = 1\n[program]\nbank = "hook_fixture"\nsha1 = "{sha}"\n'
HOOKS = HEADER + f'''
[[hook]]
address = {BASE + 0x04}
mode = "arm"
name = "HookAdd"
registers = ["r0", "r1"]

[[hook]]
address = {BASE + 0x00}
mode = "arm"
name = "HookAfterMov"
registers = ["cpsr"]
when = "after"

[[hook]]
address = {BASE + 0x24}
mode = "arm"
name = "HookSkip"
registers = ["r2"]
on_true = "jump"
jump_address = {BASE + 0x2C}

[[hook]]
address = {BASE + 0x42}
mode = "thumb"
name = "HookThumbRet"
registers = ["r0"]
on_false = "return"
'''


def recompile(out: Path, hooks: str | None, *extra: str, bank='hook_fixture'):
    out.mkdir(parents=True, exist_ok=True)
    cmd = [a.recompiler, '--config', str(work / 'fx.toml'),
           '--bin', str(work / 'fx.bin'), '--out', str(out), '--bank', bank,
           '--validate-live-bytes', *extra]
    if hooks is not None:
        (out / 'hooks.toml').write_text(hooks)
        cmd += ['--hooks', str(out / 'hooks.toml')]
    return subprocess.run(cmd, capture_output=True, text=True)


failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print('FAIL:', what, file=sys.stderr)


# Inline-leaf exclusion: fc's BL expands fa without hooks and not with them.
plain = recompile(work / 'plain', None)
check(plain.returncode == 0, 'plain recompile failed: ' + plain.stderr)
check('1 BL site expanded' in plain.stdout, 'baseline BL was not inlined')
hooked = recompile(work / 'hooked', HOOKS)
check(hooked.returncode == 0, 'hooked recompile failed: ' + hooked.stderr)
check('0 BL sites expanded' in hooked.stdout, 'hooked leaf was still inlined')
check('4 sites emitted' in hooked.stdout, 'hook site count not reported')

# Fail-closed emitter checks.
bad_cases = {
    'bank mismatch': (HOOKS, ('--bank', 'other')),
    'sha mismatch': (HOOKS.replace(sha, '0' * 40), ()),
    'address outside every function':
        (HEADER + f'[[hook]]\naddress = {BASE + 0x10}\nmode = "arm"\n'
                  'name = "Nowhere"\n', ()),
    'wrong mode':
        (HEADER + f'[[hook]]\naddress = {BASE + 0x04}\nmode = "thumb"\n'
                  'name = "WrongMode"\n', ()),
    'after a branch':
        (HEADER + f'[[hook]]\naddress = {BASE + 0x08}\nmode = "arm"\n'
                  'name = "AfterBx"\nwhen = "after"\n', ()),
    'jump outside the bank':
        (HEADER + f'[[hook]]\naddress = {BASE + 0x04}\nmode = "arm"\n'
                  'name = "FarJump"\naction = "jump"\n'
                  f'jump_address = {BASE + 0x10}\n', ()),
    'flat execution': (HOOKS, ('--flat-execution',)),
}
for what, (manifest, extra) in bad_cases.items():
    args = list(extra)
    bank = 'hook_fixture'
    if args[:1] == ['--bank']:
        bank = args[1]
        args = []
    r = recompile(work / 'bad', manifest, *args, bank=bank)
    check(r.returncode != 0, f'{what}: accepted')

# Execute the hooked bank.
out = work / 'hooked'
header = (Path(a.runtime_include) / 'runtime_arm.h').read_text()
globals_ = []
for m in re.finditer(r'^extern\s+([^;\n(]+);', header, re.M):
    decl = m[1].strip()
    if 'g_nds_active' in decl:
        continue
    if re.search(r'\w+\s*(\[[^]]*\])?$', decl):
        globals_.append(decl + ';')
includes = ['-I' + a.runtime_include, '-I' + a.core_include, '-I' + str(out)]
cflags = ['-x', 'c', '-std=gnu11', '-O1', '-DNDS_STATIC_CPU=0', *includes]
objects = []
for name in ['hook_fixture.c', 'hook_fixture_dispatch.c']:
    obj = out / (name + '.o')
    subprocess.run([a.compiler, *cflags, '-c', str(out / name), '-o', str(obj)],
                   check=True)
    objects.append(obj)

probe = (Path(__file__).with_name('hook_execution_probe.c')).read_text()
(out / 'probe.c').write_text('#include "runtime_arm.h"\n' +
                             '\n'.join(globals_) + '\n' + probe)
probe_obj = out / 'probe.o'
subprocess.run([a.compiler, *cflags, '-c', str(out / 'probe.c'),
                '-o', str(probe_obj)], check=True)

# Every runtime entry point the bank needs and the probe does not define gets
# a weak do-nothing stub, so the probe only spells out what it asserts on.
defined = set(subprocess.run(['nm', '--defined-only', '-j', str(probe_obj)],
                             capture_output=True, text=True,
                             check=True).stdout.split())
needed = set()
for obj in objects:
    needed |= set(subprocess.run(['nm', '-u', '-j', str(obj)],
                                 capture_output=True, text=True,
                                 check=True).stdout.split())
stubs = ''.join(f'__attribute__((weak)) long {s}(void) {{ return 0; }}\n'
                for s in sorted(needed - defined))
(out / 'stubs.c').write_text(stubs)
exe = out / 'hook_probe.exe'
subprocess.run([a.compiler, '-x', 'c', '-std=gnu11', '-O1', '-c',
                str(out / 'stubs.c'), '-o', str(out / 'stubs.o')], check=True)
subprocess.run([a.compiler, str(probe_obj), *map(str, objects),
                str(out / 'stubs.o'), '-o', str(exe)], check=True)
run = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stdout.write(run.stdout)
sys.stderr.write(run.stderr)
check(run.returncode == 0, 'execution probe failed')

if failures:
    sys.exit(1)
print('test_hooks: ok')
