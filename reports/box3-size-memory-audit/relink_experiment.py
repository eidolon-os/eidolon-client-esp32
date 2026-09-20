"""Read-only reuse of source build objects; all writes stay beside this script.
No configure, ninja build, source-tree editing, flashing or deployment.
"""
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
SOURCE = Path('/Users/manson/ai/eidolon/eidolon-client-esp32')
BUILD = SOURCE / 'build/eidolon/esp-box-3'
PYTHON = '/Users/manson/.espressif/python_env/idf5.5_py3.13_env/bin/python'
PREFIX = '/Users/manson/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin/xtensa-esp32s3-elf-'
CC = json.loads((BUILD / 'compile_commands.json').read_text())
LINK = shlex.split((ROOT / 'evidence/link-command.txt').read_text())[2:-2]
ARCHIVE = 'esp-idf/espressif__esp_audio_codec/libespressif__esp_audio_codec.a'

# Refuse to silently experiment against a newer or otherwise changed baseline.
manifest = json.loads((ROOT / 'evidence/manifest.json').read_text())
for name, expected in manifest.items():
    actual = hashlib.sha256((BUILD / name).read_bytes()).hexdigest()
    if actual != expected['sha256']:
        raise RuntimeError(f'Baseline changed: {name}; take a new audit snapshot first')

def run(args, log):
    log.write(shlex.join(args) + '\n')
    log.flush()
    subprocess.run(args, cwd=BUILD, stdout=log, stderr=subprocess.STDOUT, check=True)

results = {}
for variant in ['baseline-relink', 'opus-pcm-registration']:
    out = ROOT / 'experiments' / variant
    out.mkdir(parents=True, exist_ok=True)
    with (out / 'commands.log').open('w') as log:
        archive = out / Path(ARCHIVE).name
        if variant != 'baseline-relink':
            shutil.copy2(BUILD / ARCHIVE, archive)
            for name in ['audio_encoder_reg.c', 'audio_decoder_reg.c']:
                entry = next(x for x in CC if x['file'].endswith('/' + name))
                original = Path(entry['file']).read_text()
                flags = sorted(set(re.findall(r'CONFIG_AUDIO_(?:ENCODER|DECODER)_\w+_SUPPORT', original)))
                disabled = [x for x in flags if not ('_OPUS_' in x or '_PCM_' in x)]
                src = out / name
                src.write_text(original.replace('#include "sdkconfig.h"', '#include "sdkconfig.h"\n' + '\n'.join('#undef ' + x for x in disabled)))
                args = shlex.split(entry['command'])
                obj = out / (name + '.obj')
                args[args.index('-o') + 1] = str(obj)
                args[args.index('-c') + 1] = str(src)
                # The database has no dependency-output switches in this build.
                assert not any(x in args for x in ['-MD', '-MMD', '-MF'])
                run(args, log)
                run([PREFIX + 'ar', 'r', str(archive), str(obj)], log)
        args = LINK.copy()
        args[args.index('-o') + 1] = str(out / 'eidolon.elf')
        args = [('-Wl,--Map=' + str(out / 'eidolon.map')) if x.startswith('-Wl,--Map=') else x for x in args]
        if variant != 'baseline-relink':
            assert ARCHIVE in args
            args = [str(archive) if x == ARCHIVE else x for x in args]
        run(args, log)
        run([PYTHON, '-m', 'esptool', '--chip', 'esp32s3', 'elf2image', '--flash_mode', 'dio', '--flash_freq', '80m', '--flash_size', '16MB', '--elf-sha256-offset', '0xb0', '--min-rev-full', '0', '--max-rev-full', '99', '-o', str(out/'eidolon.bin'), str(out/'eidolon.elf')], log)
        for switches, name in [([], 'size.txt'), (['--archives'], 'archives.txt')]:
            with (out/name).open('w') as output:
                subprocess.run([PYTHON, '-m', 'esp_idf_size', *switches, str(out/'eidolon.map')], stdout=output, check=True)
    binary = (out/'eidolon.bin').read_bytes()
    if variant == 'baseline-relink':
        assert binary == (BUILD/'eidolon.bin').read_bytes(), 'Baseline image reproduction failed'
        assert (out/'eidolon.elf').read_bytes() == (BUILD/'eidolon.elf').read_bytes(), 'Baseline ELF reproduction failed'
    results[variant] = {'bytes': len(binary), 'sha256': hashlib.sha256(binary).hexdigest(), 'slot_free': 0x410000-len(binary)}
(ROOT/'experiments/results.json').write_text(json.dumps(results, indent=2))
print(json.dumps(results, indent=2))
