"""Headless Keil/Arm build. All outputs and configurable temp files stay on G:."""
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
MDK = ROOT / 'BOOTLOADER' / 'MDK-ARM'
OUT = ROOT / '.build' / 'BOOTLoader'
BIN = Path(r'D:\Software Download\Keil5\ARM\ARMCLANG\bin')


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    temp = ROOT / '.build' / 'tmp'
    temp.mkdir(exist_ok=True)
    env = dict(os.environ, TEMP=str(temp), TMP=str(temp))
    tree = ET.parse(MDK / 'BOOTLoader.uvprojx')
    target = tree.find('./Targets/Target')
    option = target.find('./TargetOption/TargetArmAds')
    controls = option.find('./Cads/VariousControls')
    includes = [(MDK / value).resolve() for value in
                controls.findtext('IncludePath').split(';') if value]
    defines = controls.findtext('Define').split(',')
    common = ['--target=arm-arm-none-eabi', '-mcpu=cortex-m4',
              '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard', '-c']
    objects = []
    for file in target.findall('./Groups/Group/Files/File'):
        source = (MDK / file.findtext('FilePath')).resolve()
        if source.suffix.lower() not in ('.c', '.s'):
            continue
        obj = OUT / (source.stem + '.o')
        args = [str(BIN / 'armclang.exe')] + common
        if source.suffix.lower() == '.c':
            args += ['-std=c99', '-funsigned-char', '-fshort-enums',
                     '-fshort-wchar', '-O0', '-g', '-ffunction-sections',
                     '-Werror', '-Wno-license-management']
            for include in includes:
                args += ['-I', str(include)]
            args += ['-D' + value for value in defines if value]
        else:
            args += ['-masm=auto', '-Wa,armasm,--diag_suppress=A1950W']
        args += [str(source), '-o', str(obj)]
        print('Compile:', source.name, flush=True)
        subprocess.run(args, cwd=MDK, env=env, check=True)
        objects.append(str(obj))
    scatter = MDK / 'BOOTLoader' / 'BOOTLoader.sct'
    axf = OUT / 'BOOTLoader.axf'
    map_file = OUT / 'BOOTLoader.map'
    subprocess.run([str(BIN / 'armlink.exe'), '--cpu=Cortex-M4.fp.sp',
                    '--strict', '--scatter', str(scatter), '--map',
                    '--info=sizes,totals', '--list', str(map_file),
                    '-o', str(axf)] + objects, cwd=OUT, env=env, check=True)
    subprocess.run([str(BIN / 'fromelf.exe'), '--bin',
                    '--output=' + str(OUT / 'BOOTLoader.bin'), str(axf)],
                   cwd=OUT, env=env, check=True)
    print('Built:', OUT / 'BOOTLoader.bin')
    print('Flash limit: 0x08000000 + 0xC000 (48 KB)')


if __name__ == '__main__':
    main()
